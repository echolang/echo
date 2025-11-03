# Pointers & References

If you're coming from PHP, this one might be a bit uncomfortable at first. But once you get the hang of
it, you'll see how useful pointers and references can be — and how little of C's ceremony you actually
need.

## The one big idea

In Echo, a reference is a pointer, and a pointer behaves like the value it points to.

There's no `*` to write. You never dereference by hand. A `ptr<int32>` can be read, written, added, and
passed exactly like an `int32`:

```echo
$var = 10;
$ref = &$var; // referencing $var creates a pointer to $var

$ref = 20;    // writes through the pointer, into $var

echo $ref;    // 20
echo $var;    // 20
```

That transparency is the whole point. Most of the time you can forget a pointer is a pointer.

The rest of this chapter is about the times you *can't* forget. For those there's exactly one escape
hatch: the `:$` operator.

## Reaching the pointer itself

Appending `:$` to a pointer expression gives you the address, rather than the thing at the address.

```echo
$var = 10;
$ref = &$var;

echo $ref;                  // 10 <- the value
echo $ref:$;                // error: cannot echo an address of type 'int32&' - echo prints values
```

Read `$ref:$` as "the pointer *of* `$ref`". Everything a C programmer wants to do with a pointer — compare
it, offset it, null-check it, cast it — is done on `$p:$`, never on `$p`.

`echo` prints values, so it refuses an address outright rather than printing a number you probably didn't
mean. To see that `$ref:$` really is the address, compare it:

```echo
int32& $r1 = &$var;
int32& $r2 = &$var;

dprint($r1:$ == $r2:$);     // [bool] true — the same address
dprint($r1    == $r2);      // [bool] true — and the same value
```

`:$` is a postfix operator. It binds tighter than every other operator, so `$a:$ == $b:$` compares two
addresses and `$p:$ + 1` offsets before anything else happens. It's only valid on an expression whose
type is `ptr<T>` or `T&`; writing `$x:$` on a plain `int32` is a compile error.

Why the trailing `$`? A bare `:` would be prettier, but two of them written back to back always lex as
the namespace separator `::` — which would make `$pp::` (reaching the pointer of a pointer to a pointer)
impossible to spell. Keeping the `$` puts a character between the colons, so `$pp:$:$` chains for free
with no special case. The one place to be careful is attribute values, where a space is required: write
`#[cache: $ttl]`, not `#[cache:$ttl]`.

## Two pointer types

Echo has two spellings, and they differ in exactly one respect: whether `null` is allowed.

| Type | Nullable | Written |
|---|---|---|
| `T&` | no | `int32& $r` — in parameters also `int32 &$r` |
| `ptr<T>` | yes | `ptr<int32> $p` |

Both are one machine address at runtime. Both auto-deref. The difference is what the compiler will let
you put in them:

```echo
$var = 10;

int32& $r = &$var;         // fine
int32& $bad = null;        // error: int32& cannot be null

ptr<int32> $p = &$var;     // fine
ptr<int32> $empty = null;  // fine
```

Use `T&` unless you genuinely need the absence case. Because reads auto-deref, a null `ptr<T>` faults at
the moment you *use* it, with no `*` in the source to warn you — so the type is the only warning you get.
Don't throw it away.

Converting between them:

- `T&` → `ptr<T>` happens implicitly. It's always safe.
- `ptr<T>` → `T&` needs an explicit cast, because it asserts non-nullness:
  `int32& $r = int32&($p:$);`. In a debug build this stops the program on null, with a message naming the
  file and the enclosing function. In a release build (`echoc build`, or `echoc run --release`) it's
  unchecked.

## Binding, writing, and re-seating

This is the part that trips people up, so here's the complete rule in three lines:

```echo
ptr<int32> $p = &$a;  // 1. a declaration binds the pointer
$p = 20;              // 2. an assignment writes through it, into $a
$p:$ = &$b;           // 3. an assignment to $p:$ re-seats it, now pointing at $b
```

1. **A declaration with an initializer binds.** The initializer is an address; the pointer starts out
   pointing at it.
2. **A plain assignment writes through.** `$p = 20` never changes where `$p` points; it changes what is
   *at* that address. This is what makes pointers transparent.
3. **`$p:$ = ...` re-seats.** Since `$p:$` is the address, assigning to it replaces the address.

A direct consequence: `$p = &$b;` is a compile error, not a rebind. You're assigning a `ptr<int32>` into
an `int32` slot. Write `$p:$ = &$b;`.

```echo
$a = 1;
$b = 2;
ptr<int32> $p = &$a;

$p = 100;      // $a is now 100
echo $a;       // 100

$p:$ = &$b;    // now points at $b
$p = 200;      // $b is now 200
echo $a;       // 100  <- unchanged
echo $b;       // 200
```

A `const ptr<int32>` cannot be re-seated; a `ptr<const int32>` cannot be written through. More on that
below.

## Taking addresses

`&` takes the address of anything that has one — a variable, a struct field, an element:

```echo
$var = 10;
$p1 = &$var;       // variable
$p2 = &$point->x;  // struct field
$p3 = &$buf:$[3];  // element through a pointer
```

You can't take the address of a temporary. `&($a + $b)` is an error, because there's nothing to point at,
and that stays true no matter where you write it — `inc(&5)` is refused for the same reason. Only the
*implicit* borrow at an argument position can give a value storage, and it says so by not making you
write the `&` (see *Passing to functions*).

A call result is the same story, and it's worth spelling out because the two halves look similar:

```echo
echo $o->get()->x;    // fine — reads through the address the call handed back
$r = &$o->get();      // error: Cannot take the address of an expression that has no storage
```

Reading *through* what a call returns works, because the read happens while the value is still alive.
Taking the address *of the call* does not, because a call isn't storage — there's no slot whose address
that would be.

Watch the whitespace, by the way. Echo's lexer decides between "bitwise and" and "reference" by looking
at the character right after `&`. `$a & $b` is bitwise and; `$a &$b` is a reference and almost certainly
a mistake. Always put spaces around a binary `&`, and never after a reference `&`.

## Passing to functions

A `&` parameter borrows automatically — you pass the variable, the compiler passes its address:

```echo
function inc(int32 &$x) : void {
    $x = $x + 1;   // writes through, into the caller's variable
}

$a = 5;
inc($a);       // implicit borrow
inc(&$a);      // explicit, identical
echo $a;       // 6
```

A value that has no storage at all works too. The compiler gives it a slot that lives exactly as long as
the call:

```echo
inc(41);              // a literal
inc(20 + 21);         // an arithmetic result
echo sum(Point(3, 4));  // a call result
```

Two things worth knowing about that slot.

It's destroyed as soon as the call returns, so a destructor runs there and not at the end of the scope.

And a write through it is lost. `inc(41)` really does add one, to storage nobody will ever read again.
The compiler can't tell a callee that writes from one that only reads, so it doesn't stop you. If your
parameter only reads, say so with `const int32& $x` and the question disappears.

Borrowing real storage always wins over inventing some. Where both would work, the overload taking the
variable is the one that gets picked.

A `ptr<T>` parameter does *not* auto-borrow. Because it can be null, taking an address is a decision you
should see in the source:

```echo
function maybe_inc(ptr<int32> $x) : void { ... }

maybe_inc($a);   // error: expected ptr<int32>, got int32
maybe_inc(&$a);  // fine
```

### Pointers and generics

Generic inference decays pointers to their pointee. If you pass a `ptr<int32>` to
`function box<T>(T $v)`, then `T` is `int32` — the pointer is dereferenced at the boundary, just like any
other value-position use.

```echo
function box<T>(T $v) : Box<T> { ... }

$p = &$var;
$b = box($p);   // Box<int32>, not Box<ptr<int32>>
```

To make a generic hold a pointer, ask for one explicitly:

```echo
function box_ptr<T>(ptr<T> $v) : Box<ptr<T>> { ... }
```

This rule exists to keep `Box<int32>` and `Box<ptr<int32>>` from silently becoming two different
instantiations that behave identically.

The same rule read the other way round: a borrow parameter is inferred from a value argument, because
passing a place to a borrow takes its address anyway. So a generic mutator is written the obvious way and
needs no explicit type argument:

```echo
function bump<T>(T &$v) : void { $v = $v + 1; }

$a = 1;
bump($a);       // T is int32, and $a is now 2
```

A nullable `ptr<T>` parameter is deliberately left out of both directions. It doesn't auto-borrow (see
*Passing to functions*), so there's no implicit address-of for inference to anticipate, and `hold($a)`
for a `ptr<T> $v` parameter is an error rather than a borrow you didn't write.

Both directions are top level only. Once you've asked for `ptr<T>` and passed a pointer, everything below
that match binds exactly: `ptr<T>` against a `ptr<ptr<int32>>` argument binds `T = ptr<int32>`, never
`int32`.

## Structs and classes

`->` reaches through any number of pointers automatically. There's no `.` versus `->` distinction to
remember — struct values and pointers to structs are accessed the same way:

```echo
struct Point {
    int32 $x;
    int32 $y;
}

$p = Point(1, 2);
$ref = &$p;

echo $ref->x;     // 1
$ref->x = 42;     // writes through, into $p
echo $p->x;       // 42
```

When we say a pointer is *opaque*, we mean the pointer object itself has no members or methods — not that
the pointee is out of reach. `$ref->x` is the pointee's field. There's no `$ref->address`; that's what
`$ref:$` is for.

Which is also why `$ref:$->x` is an error rather than a longer spelling of `$ref->x`. `:$` names the
pointer itself, and the pointer has no `x`. Reaching through is already what `->` does, at any depth:

```echo
ptr<Point> $items = mem::alloc<Point>(2);

$items:$[0]->x = 1;      // the first element's field
$items:$[1]->x = 2;
echo $items:$[0]->x;     // 1
```

Each operator in that chain does exactly one thing: `:$` hands over the address, `[0]` offsets it by an
element, `->x` reaches through to the field. Reading it and writing it are the same expression.

### Referencing a class

Classes are reference counted (see [Value & Reference Types](value_types.md)), and `&` over one means
something slightly different from everything above. A class handle is *already* an address, so the
address of the slot holding it is rarely what anybody wants. What they want is a second reference that
doesn't own, and that's what `&$obj` gives you where one is being asked for:

```echo
$user = User("mario");
weak<User> $watcher = &$user;   // does not retain $user
```

The destination is what decides. A `weak<T>` slot, field, parameter or return type is asking for a weak
reference and `&` supplies one. Anywhere else, `&` keeps the single meaning it has everywhere in this
chapter — which is what lets a generic container take `&` of a `T` element without its meaning depending
on what `T` turned out to be.

A weak reference doesn't dangle when its object goes away. It says so. Reading one is refused outright,
and the upgrade that reads it is the thing that can fail:

```echo
Node? $alive = strong($watcher);
```

The whole story is in [Ownership & Moving](ownership_and_moving.md#weak-references-and-cycles), including
the second count that makes it safe and the reference cycle it exists to break.

## Nullability

`null` is the null address. It has no type of its own; it takes the type of whatever it's compared or
assigned to.

```echo
ptr<int32> $empty = null;

dprint($empty:$ == null);  // [bool] true
```

Note the `:$`. You must null-check the address, not the value — `$empty == null` would auto-deref
`$empty` to read the `int32` at address zero, which is exactly the crash you were trying to avoid. The
compiler rejects comparing a pointer's *value* against `null`, so this is an error you get told about
rather than one you debug.

Two addresses compare by identity:

```echo
$var = 10;
$ref  = &$var;
$ref2 = &$var;

dprint($ref:$ == $ref2:$);  // [bool] true — same address
dprint($ref   == $ref2);    // [bool] true — same value, both 10
```

Those two lines mean genuinely different things. `$a:$ == $b:$` asks "same object?"; `$a == $b` asks
"same value?".

## Const

`const` binds to whatever it's attached to, reading outward:

```echo
ptr<const int32> $a;        // pointee is const — cannot write $a = 5, can re-seat
const ptr<int32> $b;        // pointer is const — can write $b = 5, cannot re-seat
const ptr<const int32> $c;  // neither
```

The same applies to references: `const int32& $r` is a read-only borrow, which is the form you want for
most function parameters that only need to look at their argument.

### Const methods

A method's receiver is an ordinary first parameter, so it takes a `const` the same way any other borrow
does — written ahead of `function`:

```echo
struct Box {
    int32 $value;

    constructor(int32 $v) { $this->value = $v; }

    const function get() : int32 { return $this->value; }
    function set(int32 $v) : void { $this->value = $v; }
}

const $frozen = Box(41);

echo $frozen->get();    // 41 — `get` only reads, and said so
$frozen->set(1);        // refused, by name: `set` is not declared const
```

`const function` makes `$this` a `const Box&` instead of a `Box&`, and nothing else. Everything follows
from that one type: a mutable receiver still reaches a const method (a `Box&` widens to a `const Box&`,
never the reverse), a const receiver reaches only const ones, and inside the body every write through
`$this` is refused by the ordinary const rules — including a field handed to a mutating parameter, and a
call to a method that didn't make the same promise.

Because the receiver is a parameter, a method can be overloaded on it. That's how a container offers a
writable and a read-only view of the same thing:

```echo
function at(usize $i) : T& { ... }              // chosen for a mutable receiver
const function at(usize $i) : const T& { ... }  // chosen for a const one
```

Two things are worth saying plainly.

`const` is a property of the path, not of a declaration. A field reached through a `const Box&` is const
however it was declared, and so is a field of *that* field.

And it stops at one level of indirection. Like `const` everywhere else in this chapter, it qualifies the
level it lands on. A `ptr<T>` field reached through a const receiver is a `const ptr<T>`, so it can't be
re-seated; what it points at is still writable. `ptr<const T>` is the spelling for the other half, and
it's on the field.

The one thing `const` doesn't stop is the destructor. A `const` value is still torn down at the end of
its scope: `const` is a promise about what the program writes, and the teardown is not one of the
program's writes. There's no such thing as a const destructor, and nothing needs one.

## Pointer arithmetic

Arithmetic happens on the address, so it always goes through `:$`. Offsets are scaled by the size of the
pointee, never by bytes:

```echo
ptr<int32> $p = mem::alloc<int32>(4);

$p:$[0] = 10;
$p:$[1] = 20;
$p:$[2] = 30;

$it = $p:$;      // copy the address into a new pointer

echo $it:$[0];   // 10
echo $it:$[1];   // 20

$it:$++;         // advance by one int32 — four bytes

echo $it:$[0];   // 20
echo $it:$[1];   // 30
```

`$it:$++` is a statement, not an expression — `++` and `--` always are in Echo, so `$q = $p:$++;` doesn't
parse. Advance on its own line and read on the next one.

The available operations on `$p:$`:

| Expression | Meaning |
|---|---|
| `$p:$[n]` | the element `n` positions along — an lvalue of type `T`. `$p:$[0]` is the same as `$p`. |
| `$p:$ + n`, `$p:$ - n` | a new address, offset by `n` **elements** |
| `$p:$++`, `$p:$--` | re-seat one element forward/back |
| `$p:$ - $q:$` | distance between two addresses, in elements, as `int64` |
| `$p:$ == $q:$`, `!=`, `<`, `>` | address comparison |

For byte-level arithmetic, cast to `ptr<uint8>` first — then "one element" *is* one byte.

`$p[n]` without the `:$` is an error, and that's deliberate rather than an oversight. Indexing is an
operation on the address, so it goes through `:$` like every other one. And an unqualified `[` is
reserved for the other bracket entirely: asking a *collection* for one of its elements, which is
declarable for any type and is what `$a[$i]` means over an `array<T>`. See
[Arrays](arrays.md), "The bracket, and the other bracket". Two contracts on one character would leave
you unable to tell an address operation by looking for `:$`, and that's the rule that makes pointer code
auditable.

## Casting

`ptr<U>(addr)` reinterprets an address as pointing to a different type. It takes an address, so its
argument is almost always a `:$` expression:

```echo
ptr<int32> $ints = mem::alloc<int32>(4);
ptr<uint8> $bytes = ptr<uint8>($ints:$);  // same memory, viewed as bytes

$bytes:$[0] = 0xFF;  // clobbers the low byte of $ints:$[0]
```

No alignment or size checking is performed. This is the sharpest tool in the chapter.

If the types already match, no cast is needed — `$it = $p:$` copies an address directly.

## Pointers to pointers

`ptr<T>` nests, which is what you need for out-parameters and for C interop:

```echo
function alloc_buffer(ptr<ptr<uint8>> $out) : void {
    $out = mem::alloc<uint8>(256);  // writes through into the caller's pointer
}

ptr<uint8> $buf = null;
alloc_buffer(&$buf);

$buf:$[0] = 1;
mem::free($buf);
```

No new rules here — it's the same three lines as before, one level up. Inside `alloc_buffer`:

- `$out` is the caller's `ptr<uint8>`, reached by one auto-deref. Assigning to it writes the caller's
  pointer.
- `$out:$` is the address *of* the caller's pointer. Assigning to that would re-seat `$out` itself, which
  is almost never what you want.
- `$out:$:$` chains one level further, and `$out:$[0]` is the caller's pointer again — the first element
  of a one-element array of pointers.

The classic double pointer, an argument vector:

```echo
function main(int32 $argc, ptr<ptr<uint8>> $argv) : int32 { ... }
```

## Heap allocation

Echo lets you allocate a blob of memory on the heap and get a pointer to it. This obviously isn't a safe
operation and should be used with care — nothing frees it for you.

```echo
ptr<int32> $p = mem::alloc<int32>(4);  // room for 4 int32, so 16 bytes

$p:$[0] = 424242;
$p:$[1] = 696969;

echo $p:$[0];    // 424242
echo $p:$[1];    // 696969

mem::free($p);
```

`mem::alloc<T>(n)` always takes a count of elements, never a byte count. The full surface:

| Function | Meaning |
|---|---|
| `mem::alloc<T>(count) : ptr<T>` | allocate room for `count` elements of `T`, uninitialized |
| `mem::alloc_bytes(count) : ptr<uint8>` | allocate `count` **bytes** — the untyped form |
| `mem::realloc<T>(ptr<T> $p, count) : ptr<T>` | resize; may move, returns the new address |
| `mem::free(ptr<T> $p) : void` | release. Freeing `null` is a no-op. |
| `mem::size_of<T>() : usize` | size of one `T` in bytes, as a compile-time constant |
| `mem::align_of<T>() : usize` | the alignment `T` requires, as a compile-time constant |
| `mem::copy<T>(ptr<T> $dst, ptr<T> $src, count) : void` | copy `count` elements |
| `mem::zero<T>(ptr<T> $p, count) : void` | fill `count` elements with zero bytes |

Counts are `usize`, a pointer-width integer. Every length, capacity and index in the standard library is
spelled the same way, so the signatures stay put if the target width ever changes.

`size_of<T>()` is the allocation stride, so it includes any tail padding — `size_of<T>() * n` is exactly
the room `n` elements need, and `$p:$[n]` lands on element `n`. Both it and `align_of` fold to constants
at compile time.

Because `realloc` may move the block, its result re-seats the pointer rather than being written through
it:

```echo
$p:$ = mem::realloc<int32>($p, 8);   // `:$`, not `$p = ...`
```

`$p = mem::realloc(...)` would write the new address *into* the memory `$p` points at, which is the same
rule as everywhere else — and the compiler says so rather than letting it through.

Allocation can fail; `mem::alloc` returns `null` when it does. Since it returns `ptr<T>` rather than
`T&`, the type system makes you acknowledge that before you can use the result as a reference.

Two things it deliberately doesn't offer.

There's no way to request alignment beyond what the type needs. Every block gets the C allocator's
fundamental alignment, which covers every type Echo can spell, and `align_of<T>` tells you the
requirement rather than letting you raise it.

And `count * size_of<T>()` isn't checked for overflow, so a count you computed from untrusted input can
wrap into a small allocation that you then write past. Check it yourself if it isn't a literal.

## Lifetimes, and how to get hurt

Echo doesn't track lifetimes. There's no borrow checker. Three rules keep you out of trouble:

1. **Never return a pointer to a local.** `return &$local;` is rejected by the compiler — the storage is
   gone before the caller sees it.
2. **A borrow is not an owner.** A `T&` into a struct, an array element or a heap block must not outlive
   whatever owns that storage. A *class* is the one case where the language helps: `&$obj` there is a
   `weak<T>`, which cannot dangle — it reports the object's absence instead of being read into freed
   memory.
3. **Heap memory is yours.** Every `mem::alloc` needs a matching `mem::free`, and using memory after
   freeing it is undefined.
