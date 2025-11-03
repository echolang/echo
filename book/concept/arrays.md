# Arrays

> This chapter assumes [Ownership & Moving](ownership_and_moving.md), which is where copying, moving and
> destruction are defined. It spends those rules rather than restating them.

In PHP an array is a hash map that will hold anything you put in it. In Echo it holds one type, laid
out contiguously, and — this is the part that's genuinely new — it's a value that you own.

## Three types, three jobs

The words "array", "list" and "vector" get used interchangeably in many languages, which helps nobody.
Echo has three types and each one answers a different question:

| Type | Storage | Owns it? | Grows? |
|---|---|---|---|
| `array<T>` | heap | yes | yes |
| `slice<T>` | somebody else's | no | no |
| `FixedArray<T, N>` | inline, where the value lives | yes, trivially | no |

`array<T>` is the one you want by default, and the one this chapter is mostly about. There's no
`List<T>` and no `Vector<T>` — those names are retired, not reserved.

## The one big idea

`mem::alloc<T>(n)` gives you memory. `array<T>` gives you an owner:

```echo
ptr<int32> $p = mem::alloc<int32>(4);   // memory. nothing frees it for you
$a = array<int32>();                    // an owner. it frees itself
```

Everything else in this chapter falls out of that one difference. The array knows how many elements it
holds. It knows how much room it has. It grows when you need more. And when its owner goes out of scope,
the buffer goes with it.

## Creating an array

A bracketed list is an array literal, and the elements decide the type:

```echo
$numbers = [1, 2, 3, 4, 5];       // array<int32>
$ratios  = [0.5, 1.5];            // array<float64>
```

More precisely: the first element decides it. Every element after that is fitted to the first one by an
ordinary append, so order matters at the edges:

```echo
$a = [2.5, 1];    // array<float64> — the int literal converts, as it would anywhere
$b = [1, 2.5];    // array<int32>  — and 2.5 becomes 2
```

That second one should be an error and currently isn't — the `2.5` is truncated with nothing said. Not
great. Writing the element type doesn't rescue you yet either, so if you care, put the wider element
first or construct and fill.

An empty literal has nothing to go on, so it takes its type from where it's going. That's the same
expected-type rule that types every other literal in [Expressions](expressions.md):

```echo
array<int32> $numbers = [];       // fine
$numbers = [];                    // error: nothing says what this holds
```

Or construct it and fill it:

```echo
$numbers = array<int32>();
$numbers[] = 1;
$numbers[] = 2;
```

Every literal element has to fit the one element type. There's no array of mixed types, because there's
no type that could describe one:

```echo
$mixed = [1, 2, "three"];         // error: cannot assign 'string' to 'int32'
```

A literal also works as an argument, and the compiler builds it into a name you never see:

```echo
echo total([1, 2, 3]);            // 3
```

It lives exactly as long as the statement, so one inside a loop is built and destroyed each time round.

There's one place a literal gets refused, and that's under `?->` or `??`. Neither of them evaluates its
right side, so the construction would happen on the path that was supposed to skip it:

```echo
echo pick($m ?? [1, 2]);
// error: an array literal fills storage, so it has to name it - write it as a declaration's
//        initializer or as an assignment to a variable.
```

## Elements

`$a[$i]` is the element at `$i`. It's a place — it has storage, so reading it, writing it, taking its
address and reaching through it are all the same expression:

```echo
$a[0] = 5;         // writes the element
$copy = $a[0];     // reads it: a copy of the element
$r = &$a[0];       // a pointer to the element — `$r:$` is its address
$a[0]->x = 42.0;   // `->` reaches through to a field
```

Only the first line needs any explanation, and you already have it. Indexing hands back a borrow of the
element, and assigning through a borrow writes into what it points at
([Pointers & References](pointers_and_refs_v2.md), "Binding, writing, and re-seating"). It's `$p = 20`
with an offset in front of it.

None of that is built into the language, by the way. `[` is an operator, and `array<T>` declares it the
way any type can. The whole of the element contract is one function in `stdlib/core/array.eco`:

```echo
operator<T> (array<T>& $a)[usize $index] : T&
{
    assert($index < $a->len, "array index out of range");

    return &$a->data:$[$index];
}
```

An index may be more than one operand and need not be a number, so `$m[$row, $col]` and
`$map["key"]` are the same feature. See [Operators](operators.md), "Indexing".

One thing to be deliberate about: `$copy = $a[0]` genuinely copies. `$a[0]` is a place of type `T`, not
an expression of type `T&`, so reading it reads the element rather than aliasing it. Want the alias? Ask
for it with `&`. Getting this backwards is the whole difference between a value type and a reference
type.

Indexing is bounds-checked in debug builds and unchecked in release, which is the same bargain the
`T&(addr)` cast makes. When you want the check either way, `->at($i)` always makes it.

### The bracket, and the other bracket

Echo has two ways to spell "the element at n", and they are not interchangeable:

| Written | Means |
|---|---|
| `$a[$i]` | the element of a collection |
| `$p:$[$i]` | the element `$i` along from an address |

The second is raw pointer arithmetic and belongs to the pointer chapter. The first is a collection
answering for its own storage.

A bare `$p[$i]` on a `ptr<T>` — no `:$` — is rejected, which keeps the rule from the pointer chapter
total: every operation on an address goes through `:$`, and an unqualified `[` always means "an element
of this collection."

You can index a collection a call just handed back, which is worth knowing because it's normally the
sort of thing that needs a temporary variable:

```echo
echo make()[0];       // 7 — the collection lives for the statement, then is destroyed
make()[0] = 5;
// error: 'array<int32>' has no storage of its own, so writing to it would be lost, because the
//        value is destroyed at the end of this statement. Bind it to a variable first.
```

Reading is fine, because the read happens before the value dies. Writing is refused because it would put
bytes somewhere nothing will ever read again — the same answer writing to a field of a temporary gets.

### Appending

An empty `[]` names the slot one past the end, so assigning to it appends:

```echo
$a = array<int32>();

$a[] = 10;
$a[] = 20;

dprint($a->count());   // [usize] 2
```

`$a[] = $v` is exactly `$a->push($v)`; use whichever reads better. It's the same operator with one
operand instead of two, and arity is the whole of what tells the two apart:

```echo
operator<T> (array<T>& $a)[] : T&
{
    $a->reserve($a->len + 1);
    $a->len = $a->len + 1;

    return &$a->data:$[$a->len - 1];
}
```

Three things about it are rules rather than details:

1. **The slot cannot be read.** `$v = $a[];` is an error — nothing has put a value there yet. The two
   positions that *bind* the slot rather than read it are legal: writing to it, and borrowing it with
   `&$a[]`.
2. **It's `array<T>` only.** Not because anything says so, but because a `FixedArray<T, N>` can't grow
   and a `slice<T>` doesn't own its storage, so neither declares the one-operand form. `$s[] = 1` on a
   slice is the ordinary "no overload accepts these arguments".
3. **It can move the buffer**, and therefore invalidates outstanding borrows. See
   [Growth and invalidation](#growth-and-invalidation).

`[$i]` names an element that exists and `[]` names the one that comes next, so the bracket still means
"an element of this collection" in both spellings.

## The surface

Counts and indices are `usize` throughout, matching `mem::`:

| Method | Meaning |
|---|---|
| `->count() : usize` | how many elements are in it |
| `->capacity() : usize` | how many it has room for before it grows |
| `->push($v) : void` | append one element. Same as `$a[] = $v` |
| `->push_slot() : T&` | grow by one and borrow the new, uninitialized element |
| `->pop() : T` | remove the last element and hand over its ownership |
| `->get($i) : T` | a copy of the element, bounds-checked in every build |
| `->at($i) : T&` | a borrow of the element, bounds-checked in every build |
| `->reserve($n) : void` | make room for `$n` elements without changing `count()` |
| `->clear() : void` | destroy every element; keeps the buffer |
| `->sub() : slice<T>` | a borrowed window over everything |
| `->sub($from, $len) : slice<T>` | a borrowed window over part of it |
| `->clone() : array<T>` | an explicit deep copy |

There's no `->free()`. The destructor releases the buffer at scope exit, which is the point of owning
it.

## Pushing without copying

`push` has one signature, and the parameter is a value the array takes ownership of:

```echo
function push(T $value) : void
```

No `mv` in the signature, and no overload set. The call site decides what happens, by the
place-or-non-place rule from [Ownership & Moving](ownership_and_moving.md) and nothing new:

| Written | What happens |
|---|---|
| `$a->push($m)` | `$m` is a place → the element is a **copy**; `$m` stays valid |
| `$a->push(mv $m)` | ownership transfers; `$m` is unset afterwards |
| `$a->push(Mat4x4(1.0))` | a non-place → **built directly in the slot**, no copy at all |
| `$a[] = Mat4x4(1.0)` | identical to the line above |
| `$a->push_slot()` | grow by one, borrow the slot, fill it in place |

Now here's the part worth being careful about, because the intuition is usually wrong.

Say `Mat4x4` is sixteen floats and you want a lot of them in an array. `mv` will not help you. A matrix
owns nothing, so moving it and copying it are the same 64 bytes — all `push(mv $m)` buys you is that you
can't use `$m` afterwards. What avoids the copy is never building the matrix anywhere else:

```echo
$transforms = array<Mat4x4>();
$transforms->reserve(64);              // one allocation for the whole loop

$i = 0;
while ($i < 64) {
    $transforms[] = Mat4x4(1.0);       // built in the slot. no temporary, no copy
    $i++;
}
```

`reserve` and in-place construction are doing different jobs there, and you want both. `reserve` stops
the *buffer* being copied as it grows. The non-place initializer stops the *element* being copied into
it.

Sometimes a value can't be produced by one expression and has to be filled field by field. For that,
`->push_slot()` hands back a borrow of the new element instead:

```echo
Mat4x4& $m = $transforms->push_slot();

$m->m00 = 1.0;      // written straight into the array
$m->m11 = 1.0;
```

Two cautions come with that shape. The borrow is only good until the array next grows, like every other
borrow into an array. And the slot starts out uninitialized while `count()` has already grown, so
filling it is part of the contract rather than an option — an array of half-written elements is a
problem you've made for whoever destroys it.

`->pop()` is the mirror image: the element's ownership leaves the array, so it's a move out and not a
copy.

## Growth and invalidation

An array grows by allocating a larger buffer and moving into it, doubling capacity so that a run of
appends costs an amortized constant each. `->reserve($n)` does it once, up front, when you already know
the size.

Which brings the hazard, and it's one you've met before in a different shape.

Growing an array may move its buffer, so it invalidates every borrow into it. That means:

- every `slice<T>` you took
- every `&$a[$i]`
- every `->push_slot()` result

```echo
$a = array<int32>();
$a[] = 1;

int32& $first = &$a[0];

$a[] = 2;            // may reallocate — $first may now point at freed memory

$first = 99;         // undefined. do not do this
```

This is `mem::realloc`'s rule wearing different clothes. The pointer chapter already says a realloc may
move the block, which is why its result re-seats a pointer rather than being written through it. An
`array` does the re-seating internally, so nothing in your source shows you the moment it happened — and
that's exactly why the rule has to be a habit.

Take borrows into an array after you've finished growing it, and don't keep them across an append.

This is also the rule `foreach` rests on. A loop holds a borrow into the array for its whole run, so
appending inside one ends it exactly as the code above ends `$first`. See
[Iteration](iteration.md#do-not-change-a-collection-while-you-are-iterating-it), which spends the
assumption to bind `$item` without copying.

## Ownership

An array is a value type that owns a buffer, so
[Ownership & Moving](ownership_and_moving.md) already answers every question about it:

```echo
$b = $a;               // copies the elements. two arrays, two buffers
$b = mv $a;            // transfers three fields. $a is unset
$sum = total(&$a);     // borrows. nothing is copied, nothing changes hands
return $a;             // moves out
```

`$b = $a` being a deep copy is the honest default for a value type, and it's also the expensive one.
Borrow it if the callee only reads it. Move it if you're handing it over. Reach for `->clone()` when you
want the copy to be visible at the call site rather than implied by an `=`.

One array-specific limit, and it's a sharp one: the deep copy is only correct when the element type owns
nothing itself. Copying an `array<array<int32>>` or an `array<string>` needs a recursive per-element
copy, and what happens instead is a flat copy of the elements' bytes — so two arrays end up owning one
buffer each element. It compiles, and it's wrong. Until that's fixed, keep owning types out of an array,
or move the array rather than copying it.

## Slices

A `slice<T>` is a borrowed window onto elements somebody else owns — a pointer and a length, nothing
more:

```echo
struct slice<T>
{
    ptr<T> $data;
    usize $len;
}
```

Because it owns nothing, a shallow copy is the *correct* copy. Passing a slice around duplicates two
fields and aliases the same elements, which is the whole point. It needs no destructor, and the copy
semantics that are wrong for `array<T>` today are right for `slice<T>`.

A slice is where a function should take its elements, because it doesn't care who owns them:

```echo
function total(slice<int32>& $s) : int32
{
    int32 $sum = 0;

    $i = 0;
    while ($i < $s->count()) {
        $sum = $sum + $s->get($i);
        $i++;
    }

    return $sum;
}

$whole = $a->sub();          // everything
$tail  = $a->sub(2, 2);      // two elements, starting at index 2
```

A sub-slice shares storage with its parent, so writing through one is visible through the other. And
since a slice is a borrow, the pointer chapter's second lifetime rule applies verbatim: a slice must not
outlive the array it came from, and growing that array ends the slice just as decisively as destroying
it.

Note that `->sub()` is written out rather than implicit. An `array<T>` doesn't silently decay to a
`slice<T>` at a call site. The conversion allocates nothing, but it does hand out a borrow, and handing
out a borrow is a decision you should see.

That's a choice rather than a limitation, and the hook for doing it the other way exists: a method
marked `#[implicit]` converts its type at an argument position with nothing written at the call site,
which is how a `string` binds to a `string::view` parameter. I just don't use it here.

## Fixed arrays

This one is a design, not something you can write yet — `N` is a value and type parameters are types, so
the `16` below has no spelling. It's here because the shape it describes is settled and the rest of the
chapter refers to it.

A `FixedArray<T, N>` stores its elements inline, wherever the value itself lives. No allocation, no
indirection, and a struct that has one is exactly as big as its contents:

```echo
struct Transform {
    FixedArray<float32, 16> $m;
}

FixedArray<int32, 4> $quad = [1, 2, 3, 4];
```

It owns its elements trivially, in the sense that there's no separate resource to release. So it needs
no destructor, and copying one really is a copy of its bytes. That makes it the one array flavor whose
semantics are already correct under the way the compiler copies structs today.

Indexing would work exactly as above. It doesn't grow, so `$quad[] = 5` would be an error rather than a
resize.

## Iterating

`foreach`, in the spelling PHP taught you:

```echo
foreach ($a as $item) { ... }
foreach ($a as $i => $item) { ... }
```

`$item` is the element's value — yours to change, and changing it leaves `$a` alone. When you want to
write through to the element, ask for the reference:

```echo
foreach ($a as &$item) {
    $item = $item * 2;        // $a is doubled in place
}
```

The copy the first form promises is one you rarely pay for. When nothing in the body writes `$item`, the
compiler binds `const &` and no copy happens. So a read-only loop over an array of matrices doesn't move
64 bytes per step, and you didn't have to say so.

Both `array<T>` and `slice<T>` iterate, and so does anything you write yourself. The protocol is two
ordinary [interfaces](interfaces.md), and a constrained generic dispatches to them statically at no
runtime cost. [Iteration](iteration.md) is the chapter; the one rule the loop asks you to keep is
[Growth and invalidation](#growth-and-invalidation) above.

## Arrays and pointers

The pointer chapter leaves one question to this one: does an array decay to a pointer? No. Not at a call
site, not to be helpful, not ever.

An `array<T>` is a struct with three fields, and it's not interchangeable with the address of its
elements. There's no implicit conversion in either direction, and `->data` isn't part of the surface in
the table above. The sanctioned way out is a slice, and the way to reach C is that slice's pointer:

```echo
$s = $a->sub();
some_c_function($s->data:$, $s->count());
```

That leaves exactly one place in your source — the call itself — where an owned buffer becomes a raw
address. Which is where you want it to be.
