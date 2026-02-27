# Iteration

If you've written PHP, you already know how this looks:

```echo
foreach ($a as $item) {
    echo $item;
}
```

Still feels like home, right? Good, because that's the whole surface. What's different is underneath: there
is no special array magic in there. `foreach` knows two interfaces, and anything declaring them loops exactly
as well as the standard library does.

The rest of this chapter is about what `$item` actually is, how a type of your own opts in, and the one rule
the loop asks you to keep.

## What `$item` is

By default **`$item` is the element's value** — yours to read, yours to change, and changing it doesn't touch
the collection:

```echo
foreach ($numbers as $n) {
    $n = $n * 2;          // $n is yours. $numbers is untouched
}
```

When you want to write *through* to the element, ask for a reference with `&`:

```echo
foreach ($numbers as &$n) {
    $n = $n * 2;          // $numbers is doubled in place
}
```

And when you want to be explicit that you're only reading, `const &`:

```echo
foreach ($matrices as const &$m) {
    echo $m->rows[0];     // no copy, and the compiler holds you to read-only
}
```

Three spellings, one idea: **the loop decides how the element arrives, not the collection.** The same
`array<Matrix4>` is read in one loop and written through in the next, and only the loop knows which.

Those three are the only spellings, and the compiler is picky about them for a reason:

```echo
foreach ($a as const $v) { }
// error: 'const' on a by-value binding promises nothing - the copy is already yours to write.
//        write 'const &$el' to borrow the element read-only, or drop the 'const'.

foreach ($a as &const $v) { }
// error: write 'const &$el' - the 'const' describes the element, so it goes before the '&'.
```

`const $v` is refused because a copy is already yours to write — promising not to modify your own copy
promises nothing, so if you wrote it you almost certainly meant the borrow. And `&const` is just the words in
the wrong order. Neither is a case where guessing your intent would be doing you a favour.

### The copy you do not pay for

`foreach ($matrices as $m)` says `$m` is a copy. For a `Matrix4` that reads like 64 bytes per step, and for
an `array<string>` it reads like an allocation per step. Almost always it's neither: **when nothing in the
body writes `$m`, the compiler binds `const &` instead and no copy happens.**

```echo
foreach ($matrices as $m) {
    echo $m->rows[0];     // never written -> bound as a borrow, no copy
}

foreach ($matrices as $m) {
    $m->scale(2.0);       // written -> a real copy, and $matrices is untouched
}
```

Now here's the catch, and you should know it before you lean on this: **"nothing writes it" is answered
syntactically.** The compiler has to decide before it knows which overload a call resolves to, because the
binding mode is what *decides* `$m`'s type — asking the resolver would be circular. So handing `$m` to
anything at all counts as a write, and that includes calling a method on it, because a receiver is just
argument 0:

```echo
foreach ($matrices as $m) {
    echo $m->rows[0];     // no copy — a plain property read
    echo $m->trace();     // a copy. `trace` is a const method, and it still counts
    look($m);             // a copy, even though `look` takes const Matrix4&
    dprint($m);           // a copy
}
```

So the elision is narrower than "you never pay for a copy you didn't need". What it reliably buys you is the
read-only loop that touches fields directly, plus `echo`, which is the one exception because it neither writes
nor borrows. Everything else copies.

I'd rather have it this way round. The conservative answer costs a copy; the clever answer would need
resolution to have already happened, and getting that wrong costs a miscompile. It's also monotone — every
refinement later turns a copy into a borrow, never the reverse. And if you know you want the borrow, don't
make the compiler guess: write `const &$m` and it's yours.

## Do not change a collection while you are iterating it

Appending to a collection, removing from it, or growing it **ends the loop over it** — exactly as it ends
every other borrow into it. This isn't a new rule; it's
[Growth and invalidation](arrays.md#growth-and-invalidation) wearing a different hat:

```echo
foreach ($a as $x) {
    $a[] = $x;            // undefined. do not do this
}
```

Nothing enforces it. There's no borrow checker, so this is a rule you keep rather than one you're held to —
the same bargain `slice<T>` and `&$a[$i]` already ask of you.

The loop *spends* that assumption, which is why it's worth stating plainly: it's what lets
`foreach ($a as $item)` bind a borrow instead of copying. For a program that keeps the rule, a copy and a
borrow are indistinguishable. For one that breaks it they differ — and so does every other borrow that
program is holding.

## Looping over something you may not change

A `const` collection loops. It hands you elements you may read and not write, and that shows up in which of
the three bindings you're allowed:

```echo
array<int32> $source = [1, 2, 3];
const $frozen = $source;

foreach ($frozen as $x) {          // fine - $x is your own copy
    $x = $x * 10;
}

foreach ($frozen as const &$x) {   // fine - a read-only borrow, and no copy
    echo $x;
}

foreach ($frozen as &$x) { }
// error: '$x' asks for a borrow it could write through, but 'const array<int32>' hands out
//        'const int32' elements. Write 'const &$x' to borrow it read-only, or drop the '&' for a copy.
```

Which is the same three spellings meaning the same three things — a copy is a copy whatever it came from, and
a borrow that could write through is exactly what a `const` collection has none of. Keys are unaffected:
`foreach ($frozen as $k => $v)` works, because the key arrives by value either way.

The window you get handed is a **`slice<const T>`**, and it's worth knowing the name because you can write it
down:

```echo
slice<const int32> $window = $frozen->sub();
```

The `const` is on the *elements*, and it's carried by the window's own type — so it stays true through every
function you hand it to. That's the difference from a `const` receiver, which only says it about one call.
`->sub()` and `->at()` are overloaded on the receiver in the usual way: a mutable array hands back
`slice<int32>`, a `const` one hands back `slice<const int32>`.

A generic written over `slice<T>` takes both, so you rarely need a second spelling:

```echo
function count_of<T>(slice<T> $w) : usize { ... }

count_of($frozen->sub());          // T = const int32
count_of($source->sub());          // T = int32
```

One asymmetry to know about: **a `const slice<T>` also yields `const` elements.** Read strictly, `const` on a
borrowing type would mean "you may not re-seat this window" and say nothing about what it points at — that's
what [`const` means everywhere else](pointers_and_refs_v2.md#const). Slices are the deliberate exception,
because a slice and the array behind it get used interchangeably at call sites and having them disagree about
what `const` protects would be worse than the inconsistency.

Cursors are the one thing that can't be looped over through a `const`, and it isn't a gap:

```echo
const $cursor = $source->iterate();

foreach ($cursor as $x) { }
// error: 'const slice_iterator<int32>' cannot be iterated - stepping a cursor writes to the cursor,
//        and 'contract::iterator::advance()' is not declared const.
```

A cursor is spent by being read. There is no weaker thing to hand back, so there's nothing to select.

## Keys

Some collections have something to say about *where* an element sits. Ask for it with `=>`, the same way you
would in PHP:

```echo
foreach ($a as $i => $item) {
    echo $i;
    echo ": ";
    echo $item;
}
```

A map is where a non-`usize` key is most obviously useful — see [Maps](maps.md).

The key arrives **by value** — an index, a hash key, a line number. All three element bindings work beside
it, so `foreach ($a as $i => &$item)` is the keyed form of a write-through loop.

Not every iterable has keys, and asking one that doesn't is an error at the `=>` rather than a silent zero:

```echo
foreach ($c as $k => $v) { }
// error: 'c2' iterates, but its cursor declares no key contract - so there is no '$k' to bind.
//        Declare 'contract::keyed<...>' on it, or drop the '=>'.
```

A `slice<T>` gives you indices; something like a linked list has no honest answer, so it doesn't offer one.

## Writing something you can loop over

`foreach` has no idea what an `array<T>` is. It knows a small handful of interfaces, and that's the whole
extent of it.

An **iterator** is a cursor. It knows how to step, and it knows what it's looking at:

```echo
interface contract::iterator<V>
{
    function advance() : bool;    // step to the next element; false when there are none left
    function current() : V&;      // the element it is looking at
}
```

`advance()` is called first and its answer gates the loop, so `current()` is only ever reached after a
`true`. That's the whole contract, and it's what lets `current()` skip a bounds check it would otherwise have
to repeat.

Nothing checks that your cursor terminates, by the way. An `advance()` that never returns `false` is an
infinite loop, exactly as a `while` with a condition that never falls false is.

An **iterable** is anything that can hand you a fresh cursor:

```echo
interface contract::iterable<V>
{
    type Iter : contract::iterator<V>;
    function iterate() : Iter;
}
```

`type Iter : contract::iterator<V>` is an [associated type](interfaces.md#associated-types): a type the
*implementor* chooses and the interface only constrains. You never write it — the compiler reads it off your
`iterate()`.

Put together, a collection of your own:

```echo
struct Bag : contract::iterable<int32>
{
    array<int32> $items;

    constructor() { }

    function add(int32 $v) : void {
        $this->items->push($v);
    }

    function iterate() : bag_cursor {
        return bag_cursor($this->items->sub());
    }
}

struct bag_cursor : contract::iterator<int32>, contract::keyed<usize>
{
    slice<int32> $window;
    usize $pos;                   // the index one past the current element

    constructor(slice<int32> $window) {
        $this->window = $window;
        $this->pos = 0;
    }

    function advance() : bool {
        if ($this->pos >= $this->window->count()) {
            return false;
        }

        $this->pos = $this->pos + 1;
        return true;
    }

    function current() : int32& {
        return $this->window->at($this->pos - 1);
    }

    function key() : usize {
        return $this->pos - 1;
    }
}
```

```echo
$b = Bag();
$b->add(7);
$b->add(9);

foreach ($b as $i => $v) {
    echo $i; echo ":"; echo $v; echo " ";   // 0:7 1:9
}
```

### Letting a `const` one be looped over

`Bag` above loops from a mutable value and not from a `const` one, and the diagnostic tells you exactly
that. Making it work is one more conformance:

```echo
interface contract::const_iterable<V>
{
    type Iter : contract::iterator<V>;
    const function iterate() : Iter;      // the whole difference is here
}
```

The only thing that changed is the receiver — and that's why it's a second interface rather than a second
`iterate()` overload. An interface is answered by a method making *exactly* the promise it asked for, so a
`const function iterate()` cannot answer `contract::iterable<V>` and a plain one cannot answer this. Which is
the rule doing its job: being able to call `iterate()` on a mutable value and being able to call it on a value
nobody may write are two different capabilities, and something holding the interface has to be able to tell
which one it got.

`V` means what it means above — **what the loop yields** — so you declare it over your const element type,
and the cursor you hand back is one whose `current()` returns a `const V&`:

```echo
struct Bag : contract::iterable<int32>, contract::const_iterable<const int32>
{
    // ...

    function iterate() : bag_cursor {
        return bag_cursor($this->items->sub());
    }

    const function iterate() : const_bag_cursor {
        return const_bag_cursor($this->items->sub());   // a slice<const int32>, from a const receiver
    }
}
```

`foreach` picks between the two by whether the value it was handed is `const`. A mutable value prefers the
writable contract, falling back to the read-only one — so declaring only `const_iterable` is a perfectly good
way to say "this can be looped over and never written through".

`contract::keyed<K>` is the third interface, and I kept it deliberately separate:

```echo
interface contract::keyed<K> { function key() : K; }
```

A cursor that declares it supports the `$k => $v` form; one that doesn't, doesn't. Keeping it apart from
`contract::iterator<V>` is what lets the set grow — a future `Reversible` or `RandomAccess<V>` is another
interface beside it, not a second copy of `advance` and `current`.

Declare neither, and you get told so:

```echo
foreach (42 as $v) { }
// error: 'int32' cannot be iterated - it declares neither 'contract::iterator' nor 'contract::iterable'.

foreach ($p as $v) { }
// error: 'P' cannot be iterated - it declares neither 'contract::iterator' nor 'contract::iterable'.
//        Declare one, e.g. 'struct P : contract::iterable<...>'.
```

There's nothing clever happening for primitives. `int32` isn't special-cased out of iteration; it just
doesn't declare anything, like any other type that doesn't — which is why both cases get the same error.

### Looping over a cursor directly

A `contract::iterator<V>` is iterable on its own terms. `foreach` takes one without asking for a
`contract::iterable`:

```echo
$c = $b->iterate();
$c->advance();                    // skip the first
foreach ($c as $rest) { ... }     // carry on from here
```

This is what makes adaptors possible: a filter or a map is a struct holding another cursor, declaring
`contract::iterator<V>`, and `foreach` can't tell the difference.

One consequence of `current()` returning `V&`: a cursor producing a *computed* value — a range, a map
adaptor — has to materialize it into a field of its own and hand back a borrow of that. It works, and it
costs one slot.

### Looping over an erased iterator

A **class** implementing `contract::iterator<V>` can be stored as one and iterated through the vtable:

```echo
contract::iterator<int32> $source = make_some_source();
foreach ($source as $v) { ... }
```

The loop is identical; the two calls per step become indirect. This is the one place iteration costs more
than a hand-written loop, and it's what you asked for by erasing.

An erased `contract::iterable<V>` is *not* iterable, and the error says so. Its iterator's type is chosen at
the moment of erasure and the vtable doesn't carry it, so there's nothing for `iterate()` to return. Erase
the cursor, not the collection.

## What it costs

Nothing, in a release build, over the cursor-and-`while` you'd have written yourself.

`advance()` and `current()` on a struct cursor are direct calls, so they inline; the bounds `assert` in the
element operator is compiled out entirely by `echoc build`; and the `+1`/`-1` around `pos` folds away. What's
left is one compare, one branch, one increment and one indexed load per step — the same loop, and the same
vectorization.

Two shapes do cost more, and both are visible in the source:

- **A debug build** doesn't inline, so a step is two real calls. That's true of `$a[$i]` today as well; it's
  the price of `echoc run`, not of iteration.
- **An erased iterator** dispatches indirectly, as above.

If you're writing a cursor of your own and you care about this: **hold your bound by value.** A cursor
keeping a `slice<T>` has its length in a register and the loop's trip count is known; one keeping a
`Collection&` and calling `->count()` each step forces a reload the optimizer can't see through.
