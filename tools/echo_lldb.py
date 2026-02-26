"""lldb data formatters for Echo's standard library.

`echoc build -g` puts complete and honest DWARF in the object, and a container still reads as its
internals: an `array<string>` shows a buffer pointer and a length, a `map<K,V>` shows three raw slot
arrays, and only the *first* element of either is reachable at all.

**That is not a gap in the debug info and cannot be closed by emitting more of it.** DWARF describes
layout. It has no way to say "the length is in `len` and the elements are at `storage.data[0..len)`",
and none at all for a hash table's occupancy rule - its dynamic-array features are Fortran-oriented
and lldb's support for them is partial. Every language solves this the same way, one layer up: libc++,
Rust and Swift all ship formatters, and this is Echo's.

    (lldb) command script import tools/echo_lldb.py

Everything registers into the `echo` type category, so `type category disable echo` gives the raw
view back. See notes/debuginfo.md for why this layer exists and book/concept/debugging.md for using it.

**Two things a reader of this file has to know before changing anything.**

A provider runs on values that are *not initialized yet* - a local inspected in a prologue, a
`Foo $x;` whose zero-init has not run, a frame the program has not reached. A garbage `len` of 2^60
turns the variables panel into a hang and a garbage pointer into a crash report filed against lldb.
So every provider here obeys the four rules in the "SAFETY" block below, and none of them is optional.

And `update()` must return **False**. True tells lldb the child list may be cached across stops, which
is wrong for all of these: a container mutates while the program runs, and a cached one shows the
shape it had at the last breakpoint.
"""

import lldb

# ---------------------------------------------------------------------------------------------
# SAFETY - the four rules, in one place because every provider below depends on all of them
#
#  1. guard null pointers and zero capacities *first*. Both are legitimate empty states - a
#     `map<K,V>` fresh from its constructor has three null pointers and `cap == 0` - so they yield
#     zero children rather than an error.
#  2. cap every count. Anything above these ceilings is read as "this value is not initialized",
#     which is reported rather than shown: a wrong number said out loud beats a wrong number drawn.
#  3. check every read. An SBError that is ignored becomes a Python traceback printed into the
#     variables panel, which is worse than the unformatted rendering it replaced.
#  4. never raise. `_guard` turns any escaped exception into the fallback, for rule 3's reason.
# ---------------------------------------------------------------------------------------------

MAX_ELEMENTS = 100_000      # a sequence longer than this is not a sequence, it is uninitialized memory
MAX_SLOTS = 1 << 22         # map capacity is a power of two; 4M slots is already 32MB of hashes alone
MAX_TEXT = 4096             # a string summary is a preview, not a document
PREVIEW_ITEMS = 4           # elements shown inline before the summary gives up and counts instead
PREVIEW_ENTRIES = 3         # the same for a map, which is wider per entry

CATEGORY = "echo"

# the reference-counting words a class box carries in front of its payload. Hidden from the child
# list and gathered under `[refcount]` instead - they are how a class is *stored*, and a person
# reading their own object should not have to look past them to find their own fields
BOX_HEADER = ("__strong", "__weak", "__typeinfo")


def _summary(fn):
    """Decorator for a summary function: never raise, and **keep the two-argument signature.**

    lldb introspects the arity of a summary callable to decide how to call it, so a wrapper declared
    `(*args, **kwargs)` is not recognised as one and the rule is **skipped in silence** - registered,
    listed by `type summary list`, and never invoked. That is why this cannot share `_guard` below,
    whose wrapper is variadic because it stands in for methods of five different shapes."""
    def call(valobj, internal_dict):
        try:
            return fn(valobj, internal_dict)
        except Exception:
            return ""

    call.__name__ = fn.__name__
    call.__doc__ = fn.__doc__

    return call


def _guard(fallback):
    """Decorator: never let an exception escape a formatter.

    **The failure this prevents is permanent, not a one-off.** A traceback out of a provider is
    printed by lldb into `frame variable` at *every* stop, and that value never renders again for the
    rest of the session - so an exception is strictly worse than the unformatted output it replaced."""
    def wrap(fn):
        def call(*args, **kwargs):
            try:
                return fn(*args, **kwargs)
            except Exception:
                return fallback
        return call
    return wrap


def _safe_provider(cls):
    """Applies `_guard` to every method lldb calls on a synthetic provider.

    A class decorator rather than four decorators per class, because "which methods can lldb call"
    is one fact and a provider added later must not have to remember it. The fallbacks are the
    not-trustworthy answers from the SAFETY block: no children, and nothing at any index."""
    # **`num_children` takes an optional bound.** lldb 20 passes a `max_children` argument when the
    # callable accepts one, and `_guard`'s wrapper is variadic - so it accepts one, forwards it, and a
    # provider declaring `num_children(self)` raised a TypeError that the guard turned into zero
    # children. Every container rendered empty, with nothing anywhere saying why
    for name, fallback in (
            ("update", False), ("num_children", 0),
            ("get_child_at_index", None), ("get_child_index", -1),
            ("has_children", False)):
        if hasattr(cls, name):
            setattr(cls, name, _guard(fallback)(getattr(cls, name)))

    return cls


def _member(value, *path):
    """A member by name down a path, or None.

    **Through `GetNonSyntheticValue()`, which is not optional.** Once a type has a synthetic provider,
    `GetChildMemberWithName` on one of its values searches the *synthetic* child list - so a provider
    asking its own value for `storage` is asking the children it has not built yet, and gets an
    invalid value back. Every container here then reported zero elements, and the summaries that
    swallowed the resulting exception rendered `{}`: a formatter that looks like an empty container.
    A non-synthetic value answers this unchanged, so it is safe everywhere and stated once here.

    Named lookup rather than by index, because AST::resolve_core_string_layout already establishes
    that these layouts are keyed on names - the compiler resolves them the same way."""
    for name in path:
        if not value or not value.IsValid():
            return None
        value = value.GetNonSyntheticValue().GetChildMemberWithName(name)
    return value if value and value.IsValid() else None


def _unsigned(value, default=0):
    return value.GetValueAsUnsigned(default) if value and value.IsValid() else default


def _display(value):
    """What a child looks like inside a parent's one-line summary."""
    if not value or not value.IsValid():
        return "?"
    return value.GetSummary() or value.GetValue() or "?"


def _preview(value, limit, open_br, close_br, render):
    """`[1, 2, 3, ...] (1000 items)` - the shared shape of every container summary here.

    Truncated rather than complete on purpose: a summary is drawn on hover and in a watch list, and
    a thousand-element read at that moment is a stall the person did not ask for."""
    count = value.GetNumChildren()
    shown = [render(value.GetChildAtIndex(i)) for i in range(min(count, limit))]

    body = ", ".join(shown)
    if count > limit:
        body += ", ..."

    tail = "" if count <= limit else " (%d items)" % count

    return "%s%s%s%s" % (open_br, body, close_br, tail)


# ---------------------------------------------------------------------------------------------
# string and string::view
# ---------------------------------------------------------------------------------------------

def _read_text(view):
    """The bytes a `string::view` names, as text.

    **Length-delimited, never a C-string read.** A substring shares its owner's buffer and narrows
    only the window (`string::sub`), so the bytes are not NUL-terminated at `size` - `view::is_terminated`
    exists precisely because a window that stops early is not. lldb renders `ptr<const uint8>` as a
    C string by default, which is why the *unformatted* output looks right and is wrong: on
    `$s->sub(0, 3)` it prints the whole of "Alice" where the answer is "Ali"."""
    bytes_ptr = _member(view, "bytes")
    size = _member(view, "size")

    if bytes_ptr is None or size is None:
        return None

    address = _unsigned(bytes_ptr)
    length = _unsigned(size)

    if address == 0:
        return "" if length == 0 else None

    if length > MAX_TEXT:
        return None

    error = lldb.SBError()
    raw = view.GetProcess().ReadMemory(address, length, error)

    if not error.Success() or raw is None:
        return None

    return raw.decode("utf-8", "replace")


def _quote(text):
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")


@_summary
def string_summary(value, _internal):
    text = _read_text(_member(value, "window"))
    return _quote(text) if text is not None else "<unreadable>"


@_summary
def view_summary(value, _internal):
    text = _read_text(value)
    return _quote(text) if text is not None else "<unreadable>"


# ---------------------------------------------------------------------------------------------
# array<T>, slice<T> - a base pointer and a length, reached differently
# ---------------------------------------------------------------------------------------------

@_safe_provider
class SequenceProvider:
    """`array<T>` keeps its elements in a `mem::buffer<T>` and `slice<T>` points straight at them,
    so the only difference between the two is where the base pointer is - which `_base_pointer`
    below is the whole of."""

    def __init__(self, value, _internal):
        self.value = value
        self.count = 0
        self.base = 0
        self.element_type = None
        self.element_size = 0

    def _base_pointer(self):
        # array<T> first, because a slice has no `storage` and an array has no top-level `data`
        return _member(self.value, "storage", "data") or _member(self.value, "data")

    def update(self):
        self.count = 0

        data = self._base_pointer()
        length = _member(self.value, "len")

        # not one of ours: some other type matched the name regex. Falling back to zero children
        # leaves lldb's own rendering in place rather than inventing elements out of a stranger's bytes
        if data is None or length is None:
            return False

        self.base = _unsigned(data)
        self.element_type = data.GetType().GetPointeeType()
        self.element_size = self.element_type.GetByteSize() if self.element_type.IsValid() else 0

        count = _unsigned(length)

        # rule 1 and rule 2 together: an empty array is `data == null, len == 0`, and anything past
        # the ceiling is uninitialized storage rather than a length
        if self.base == 0 or self.element_size == 0 or count > MAX_ELEMENTS:
            return False

        self.count = count
        return False

    def num_children(self, max_count=None):
        return self.count

    def get_child_index(self, name):
        try:
            return int(name.strip("[]"))
        except ValueError:
            return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= self.count:
            return None

        return self.value.CreateValueFromAddress(
            "[%d]" % index, self.base + index * self.element_size, self.element_type)


@_summary
def sequence_summary(value, _internal):
    if value.GetNumChildren() == 0 and _unsigned(_member(value, "len")) > MAX_ELEMENTS:
        return "<uninitialized?>"

    return _preview(value, PREVIEW_ITEMS, "[", "]", _display)


@_summary
def buffer_summary(value, _internal):
    """**No elements, deliberately.** A `mem::buffer<T>` is raw storage with a capacity and no
    length - nothing in it says which slots hold a live value, so any element shown would be
    invented. The owner (`array<T>`, `map<K,V>`) is what knows, and formats them."""
    return "<buffer cap %d>" % _unsigned(_member(value, "cap"))


# ---------------------------------------------------------------------------------------------
# map<K, V> - three parallel arrays, with the slot state encoded in the hash word
# ---------------------------------------------------------------------------------------------

@_safe_provider
class MapProvider:
    """**The slot state is the hash: 0 empty, 1 tombstone, >= 2 occupied.** `map::slot_hash` seals a
    real hash up into 2 and above precisely so the two sentinels are free, and `slot_is_live` is the
    `> 1` this mirrors. Reading it any other way - a separate liveness array, a null key - would be a
    second answer to a question the library already owns."""

    def __init__(self, value, _internal):
        self.value = value
        self.slots = []

    def update(self):
        self.slots = []

        hashes = _member(self.value, "hashes")
        keys = _member(self.value, "keys")
        values = _member(self.value, "values")
        cap = _member(self.value, "cap")

        if None in (hashes, keys, values, cap):
            return False

        hashes_at, keys_at, values_at = _unsigned(hashes), _unsigned(keys), _unsigned(values)
        capacity = _unsigned(cap)

        # a map that has never seated anything holds three nulls and a zero capacity - the
        # constructor's own state, and an empty map rather than a broken one
        if capacity == 0 or 0 in (hashes_at, keys_at, values_at) or capacity > MAX_SLOTS:
            return False

        self.key_type = keys.GetType().GetPointeeType()
        self.value_type = values.GetType().GetPointeeType()
        key_size = self.key_type.GetByteSize() if self.key_type.IsValid() else 0
        value_size = self.value_type.GetByteSize() if self.value_type.IsValid() else 0

        if key_size == 0 or value_size == 0:
            return False

        # the whole hash array in one read rather than one read per slot: a probe-length map is
        # mostly empty, and a round trip per empty slot is what makes a formatter feel broken
        error = lldb.SBError()
        raw = self.value.GetProcess().ReadMemory(hashes_at, capacity * 8, error)

        if not error.Success() or raw is None:
            return False

        for slot in range(capacity):
            word = int.from_bytes(raw[slot * 8:(slot + 1) * 8], "little")

            if word > 1:
                self.slots.append(
                    (keys_at + slot * key_size, values_at + slot * value_size))

        return False

    def num_children(self, max_count=None):
        return len(self.slots)

    def get_child_index(self, _name):
        # a map's children are named after their keys, which are not indices - so lldb is told to
        # search rather than compute, and `v m["k"]` is simply not a path it can take
        return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= len(self.slots):
            return None

        key_at, value_at = self.slots[index]
        key = self.value.CreateValueFromAddress("key", key_at, self.key_type)

        return self.value.CreateValueFromAddress(
            "[%s]" % _display(key), value_at, self.value_type)


@_safe_provider
class OrderedMapProvider:
    """`ordered_map<K,V>` is a `map<K,V>` plus an `array<K>` of keys in insertion order.

    **Walked through the order array**, not the table, because insertion order is the entire reason
    the type exists - a person who reached for it and then saw slot order in the debugger would have
    to wonder which one the program sees."""

    def __init__(self, value, _internal):
        self.value = value
        self.table = MapProvider(value, None)
        self.order = None
        self.by_label = {}

    def update(self):
        table = _member(self.value, "table")
        order = _member(self.value, "order")

        self.order = None
        self.by_label = {}

        if table is None or order is None:
            return False

        self.table = MapProvider(table, None)
        self.table.update()

        # the order array's own elements, through the sequence provider so the two agree about
        # what an out-of-range or uninitialized length means
        sequence = SequenceProvider(order, None)
        sequence.update()
        self.order = sequence

        # the table's seated entries by their rendered key, in one pass. A hash lookup would mean
        # re-implementing `hash::of` for every K in Python, which is a second answer to a question
        # the program already answers - but matching the rendered key does not need a scan per
        # child, and one did make expanding an n-entry map cost n**2 target reads
        for slot in range(self.table.num_children()):
            child = self.table.get_child_at_index(slot)

            if child is not None:
                self.by_label[child.GetName()] = child

        return False

    def num_children(self, max_count=None):
        return self.order.num_children() if self.order else 0

    def get_child_index(self, _name):
        return -1

    def get_child_at_index(self, index):
        if not self.order or index < 0 or index >= self.order.num_children():
            return None

        key = self.order.get_child_at_index(index)
        label = _display(key)

        # the value for this key, by the rendered key the table already labelled it with. A key the
        # order array names and the table does not is the map mid-write, and the key itself is the
        # honest thing to show for it
        return self.by_label.get("[%s]" % label, key)


@_summary
def map_summary(value, _internal):
    def entry(child):
        return "%s: %s" % (child.GetName().strip("[]"), _display(child)) if child else "?"

    return _preview(value, PREVIEW_ENTRIES, "{", "}", entry)


# ---------------------------------------------------------------------------------------------
# T? - the wrapped optional. `{ i1 __has, T __value }`
# ---------------------------------------------------------------------------------------------

@_summary
def optional_summary(value, _internal):
    has = _member(value, "__has")

    if has is None:
        return ""

    # **three answers, not two.** `__has` is one byte, and an uninitialized optional holds whatever
    # was on the stack - so anything outside {0, 1} is a slot nobody wrote, and saying so beats
    # printing the garbage in `__value` as though it were a number somebody chose
    present = _unsigned(has)

    if present > 1:
        return "<uninitialized?>"

    return _display(_member(value, "__value")) if present else "null"


@_safe_provider
class OptionalProvider:
    """One child, the value, and only when there is one. An absent optional showing a `__value`
    full of whatever was on the stack is the shape of lie this whole file exists to stop."""

    def __init__(self, value, _internal):
        self.value = value
        self.present = False

    def update(self):
        has = _member(self.value, "__has")
        self.present = has is not None and _unsigned(has) == 1
        return False

    def num_children(self, max_count=None):
        return 1 if self.present else 0

    def get_child_index(self, name):
        return 0 if name == "value" else -1

    def get_child_at_index(self, index):
        if index != 0 or not self.present:
            return None

        member = _member(self.value, "__value")
        return member.Clone("value") if member else None


# ---------------------------------------------------------------------------------------------
# classes - a pointer to a heap box, refcount header in front of the payload
# ---------------------------------------------------------------------------------------------

def _box(value):
    """The box itself, whether the value is the handle or already the block."""
    return value.Dereference() if value.TypeIsPointerType() else value


@_safe_provider
class ClassProvider:
    """The declared properties, then one `[refcount]` child.

    `DIFlagArtificial` on the three header words does **not** hide them from lldb's struct rendering
    - verified - so the split is done here. Gathered rather than dropped: a leak hunt wants exactly
    those numbers, and hiding storage the program has is the category of lie C6 is about."""

    def __init__(self, value, _internal):
        self.value = value
        self.payload = []
        self.header = []

    def update(self):
        self.payload = []
        self.header = []

        if self.value.TypeIsPointerType() and _unsigned(self.value) == 0:
            return False

        box = _box(self.value.GetNonSyntheticValue())

        if not box or not box.IsValid():
            return False

        for index in range(box.GetNumChildren()):
            child = box.GetChildAtIndex(index)
            bucket = self.header if child.GetName() in BOX_HEADER else self.payload
            bucket.append(child)

        # **the shape check, and the whole reason the regex above may be broad.** A class box is named
        # after its class, which is any identifier at all - there is nothing in a DWARF name that tells
        # `Node` (a class) from `Point` (a struct) or from a `Point&` parameter, which is also a
        # pointer. The three header words are what does, and a value that has none of them is presented
        # exactly as lldb would have: every child, in order, no summary. So a struct that falls through
        # this regex renders unchanged rather than as an empty aggregate - which is what an empty
        # `header` already gives it, below, with nothing to do here

        return False

    def num_children(self, max_count=None):
        return len(self.payload) + (1 if self.header else 0)

    def get_child_index(self, name):
        for index, child in enumerate(self.payload):
            if child.GetName() == name:
                return index

        return len(self.payload) if name == "[refcount]" and self.header else -1

    def get_child_at_index(self, index):
        if index < len(self.payload):
            return self.payload[index]

        if index == len(self.payload) and self.header:
            # the strong count stands for the group: lldb has no synthetic *struct* to nest the
            # three in, and the strong count is the one a person is looking for when they look at all
            return self.header[0].Clone("[refcount]")

        return None


@_summary
def class_summary(value, _internal):
    if value.TypeIsPointerType() and _unsigned(value) == 0:
        return "null"

    # not a class: no header words. An empty summary is how lldb is told to render this the way it
    # would have, which is what keeps the broad regex from restyling every plain struct
    if _member(value, "__strong") is None:
        return ""

    name = value.GetType().GetName().replace(" *", "")

    fields = []
    for index in range(min(value.GetNumChildren(), PREVIEW_ITEMS)):
        child = value.GetChildAtIndex(index)

        if child.GetName() == "[refcount]":
            continue

        fields.append("%s = %s" % (child.GetName(), _display(child)))

    return "%s(%s)" % (name, ", ".join(fields))


# ---------------------------------------------------------------------------------------------
# registration
# ---------------------------------------------------------------------------------------------

# **anchored, and matching the Echo spelling.** These are ComplexType::namespaced_name(), which is
# what a person wrote: `array<int32>`, `map<string,int32>` with no space after the comma, `int32?`
# for a wrapped optional. A nested instantiation gives `array<array<int32>>`, so `.+` is greedy on
# purpose - the trailing `>>` belongs to the argument.
#
# a class is matched by *no* pattern: its box is named after the class, which is any identifier at
# all. The `-x` regex below is deliberately broad and every provider re-checks the shape it needs,
# which is what keeps a user type called `array` from being formatted as one.
SUMMARIES = [
    ("string", "echo_lldb.string_summary", False),
    ("string::view", "echo_lldb.view_summary", False),
    (r"^array<.+>$", "echo_lldb.sequence_summary", True),
    (r"^slice<.+>$", "echo_lldb.sequence_summary", True),
    (r"^mem::buffer<.+>$", "echo_lldb.buffer_summary", True),
    (r"^ordered_map<.+>$", "echo_lldb.map_summary", True),
    (r"^map<.+>$", "echo_lldb.map_summary", True),
    (r"^.+\?$", "echo_lldb.optional_summary", True),

    # **a class is matched by shape, not by name.** Its box is named after the class, so the pattern
    # is "a bare identifier, optionally a pointer to one" - which also catches every plain struct and
    # every borrow parameter. ClassProvider and class_summary both check for the reference-count
    # header and present anything without one exactly as lldb would, so the breadth costs nothing.
    # No `<` or `?` can appear, which is what keeps this off the generic instantiations above
    (r"^[A-Za-z_][A-Za-z0-9_:]*( \*)?$", "echo_lldb.class_summary", True),
]

SYNTHETICS = [
    (r"^array<.+>$", "echo_lldb.SequenceProvider"),
    (r"^slice<.+>$", "echo_lldb.SequenceProvider"),
    (r"^ordered_map<.+>$", "echo_lldb.OrderedMapProvider"),
    (r"^map<.+>$", "echo_lldb.MapProvider"),
    (r"^.+\?$", "echo_lldb.OptionalProvider"),
    (r"^[A-Za-z_][A-Za-z0-9_:]*( \*)?$", "echo_lldb.ClassProvider"),
]


def __lldb_init_module(debugger, _internal):
    def run(command):
        debugger.HandleCommand(command)

    # a re-import must not stack a second copy of every rule on top of the first, and on a *first*
    # import there is no category to drop - so this one goes through the interpreter directly, whose
    # result object absorbs the "cannot delete one or more categories" that HandleCommand would
    # otherwise print at every clean session
    debugger.GetCommandInterpreter().HandleCommand(
        "type category delete %s" % CATEGORY, lldb.SBCommandReturnObject())

    for pattern, function, is_regex in SUMMARIES:
        run('type summary add -F %s -w %s %s "%s"'
            % (function, CATEGORY, "-x" if is_regex else "", pattern))

    for pattern, provider in SYNTHETICS:
        run('type synthetic add -l %s -w %s -x "%s"' % (provider, CATEGORY, pattern))

    run("type category enable %s" % CATEGORY)
