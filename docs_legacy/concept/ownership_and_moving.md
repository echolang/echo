# Ownership & Moving

PHP never asks who owns a value. The runtime owns everything and tidies up behind you, so the question
never comes up. Echo has no runtime, which means the question has an answer, the answer is in your
source, and this chapter is about learning to read it.

The good news: there's one rule, and it's built out of a distinction you already know.

## One owner, one destruction

Every value has exactly one owner — the variable, field or element that holds it. When the owner goes out
of scope the value is destroyed, and locals are destroyed in reverse declaration order:

```echo
function main() : int32
{
    $a = array<int32>();   // $a owns a buffer
    $b = array<int32>();   // so does $b

    return 0;
}                          // $b is destroyed, then $a
```

For an `int32`, "destroyed" means nothing happens at all. The stack slot goes away and that's the whole
story.

Destruction only becomes visible for a type that owns something beyond its own bytes: a heap buffer, a
file handle, a lock. Such a type says what to do in a `destructor`, and the rest of this chapter is about
making sure that destructor runs exactly once.

A struct that *contains* an owner is itself an owner, and you don't declare anything for that:

```echo
struct Document {
    array<uint8> $body;   // Document owns whatever $body owns
    usize $version;
}
```

Ownership isn't a property you opt into. It's what having a field means.

## The one big idea

Handing a value somewhere else is one of exactly two things:

- a **copy** — both are valid afterwards, and you paid for the duplicate;
- a **move** — the destination is valid, the source is not, and you paid almost nothing.

Echo picks between them by asking a question it already knows how to ask: *does this expression have
storage?* An expression that does is a place, and a place is precisely the thing you're allowed to
write `&` in front of (see [Pointers & References](pointers_and_refs_v2.md), "Taking addresses").

```echo
$b = $a;              // $a is a place              -> copy. $a is still valid
$b = mv $a;           // you asked for a move       -> transfer. $a is now unset
$b = make_matrix();   // a call is not a place      -> move. there is nothing to copy
$b = Mat4x4(1.0);     // a constructor is not a place -> built directly into $b
```

A place is copied. A non-place is moved. Every rule below is a consequence of that one line:

1. **Returning a local moves it.** The caller's slot is the destination, the local is about to die, so
   there's nothing to duplicate and nothing left behind to destroy.
2. **A temporary argument moves.** `consume(make_matrix())` hands over a value nobody else holds.
3. **`mv` is only ever needed on a place** — which is exactly the situation where a reader needs to be
   told that a variable whose name they can still type has stopped being valid.
4. **A non-place initializer isn't even moved. It's built where it belongs.** A move still copies bytes;
   construction in place copies none. This is the difference that matters most in practice, and it gets
   [its own section](#building-in-place) below.

So why require `mv` at all, when the compiler could just infer the last use of a variable?

Because the source stays in scope with a name you can still type. Inferring it would make `$a`'s validity
depend on whether some line further down happens to mention it — so adding an `echo $a;` at the bottom of
a function would silently turn a move near the top into a copy. That's the same reasoning that makes
`$p = &$b;` an error rather than a quiet rebind. A transfer of ownership is a thing you should be able to
see.

## Moving a place

`mv` is a prefix operator. It says "take this value out of here":

```echo
$a = array<int32>();
$a[] = 42;

$b = mv $a;      // $b now owns the buffer
echo $b[0];      // 42
echo $a[0];      // error: '$a' has been moved out of.
```

After a move the source is **unset**. It's not empty, not null, not zero — it has no value, and any use
of it is a compile error rather than something you discover at runtime. A moved-from local also isn't
destroyed at the end of its scope; its destructor travelled with the value.

Moving on one branch and not the other is refused outright, at the `mv`, rather than at some later read:

```echo
if ($flag) {
    $b = mv $a;
}
// error: '$a' owns a resource and is moved out of on only one branch, so nothing would destroy it
//        on the other. Move it on every branch, or after the 'if'.
```

The problem isn't the read afterwards. It's the branch that *didn't* move — nothing would ever destroy
the value on that path. Answering that properly needs a runtime flag per conditionally-moved local, and
I'd rather refuse a program than start carrying hidden bookkeeping around. Move on every branch, or move
after the `if`.

A value that owns nothing is free to do either, because there's no destruction to decide about.

The same reasoning refuses a move inside a loop, from the other direction:

```echo
while ($more) {
    $b = mv $a;
}
// error: '$a' is moved out of inside a loop, so the next iteration would move a value that is no
//        longer there. Move it after the loop, or declare it inside one.
```

### Moving into a function

A parameter marked `mv` takes ownership of its argument:

```echo
function consume(mv array<int32> $items) : void
{
    // $items is ours; it is destroyed at the end of this body
}

$a = array<int32>();

consume($a);        // error: consume takes ownership, write `mv $a`
consume(mv $a);     // fine — and $a is unset from here on
consume(make());    // fine — a temporary, so no `mv` to write
```

The error on the first call is the whole point of the annotation. A function that quietly swallowed its
argument would be indistinguishable at the call site from one that borrowed it.

### Moving out of a function

Returns need no annotation at all, because there's nothing else a returned local could be:

```echo
function make() : array<int32>
{
    $a = array<int32>();
    $a[] = 1;

    return $a;      // a move. always
}
```

Why no `mv` on the return type? Because it would never be optional. A local is destroyed at the end of
its scope, and `return` *is* the end of its scope — so a returned local either moves or it's destroyed
out from under the caller. Writing `: mv array<int32>` would decorate the only possible behavior, which
is noise.

## `mv` is not the same as cheap

This one trips everybody, so I'll be blunt about it: moving a value that owns nothing costs exactly what
copying it costs.

A `Mat4x4` is sixteen floats and no allocation. `mv` transfers ownership of nothing, and still writes 64
bytes:

```echo
$b = mv $matrix;   // still 64 bytes. mv changed the bookkeeping, not the work
$b = mv $array;    // three fields. the elements did not move at all
```

`mv` buys you something only when the value owns a resource, because then the transfer is a handful of
fields where a copy would have been a whole buffer. For a plain aggregate it buys you nothing but a
stricter source.

So if the matrix is what you're worried about, `mv` is the wrong tool. The right one is never building it
in a second place to begin with.

## Building in place

A non-place initializer is constructed directly into its destination. There's no temporary and no copy —
the constructor writes its result where the value is going to live:

```echo
Mat4x4 $m = Mat4x4(1.0);      // built in $m's storage
```

The same applies at all three destinations a value can arrive at:

| Destination | Written | What happens |
|---|---|---|
| a declaration | `Mat4x4 $m = Mat4x4(1.0);` | built in `$m` |
| an assignment | `$m = Mat4x4(1.0);` | the old value is destroyed, the new one is built in place |
| an argument | `consume(Mat4x4(1.0))` | built in the parameter's storage |

Which is also why the chapter's advice comes in this order:

1. **Borrow it** if the callee only needs to look at it or change it — nothing is copied and nothing
   changes hands.
2. **Build it in place** if you're creating it, so there's no second copy to avoid.
3. **Move it** if you have one already and want to hand it over.
4. **Copy it** only when you genuinely want two.

### Filling a value that cannot be returned whole

Building in place needs the value to be the result of one expression. When it has to be filled field by
field, reach for a function that hands back a borrow of storage that already exists:

```echo
Mat4x4& $m = $transforms->push_slot();   // grow by one, borrow the new element

$m->m00 = 1.0;                           // written straight into the array
$m->m11 = 1.0;
```

Nothing is constructed anywhere else, so nothing is copied in. The array's `->push_slot()` is exactly
this ([Arrays](arrays.md), "Pushing without copying"). The two cautions there — the borrow is only good
until the container next grows, and the storage starts out uninitialized — come with the shape rather
than with arrays.

## Borrowing instead

Most values that get passed around don't need to change owner at all. A borrow transfers nothing and
copies nothing:

```echo
function total(array<int32>& $items) : int32 { ... }

$a = array<int32>();
$sum = total($a);      // implicit borrow. $a is untouched and still ours
```

`&` parameters borrow automatically, so this reads exactly like passing by value and costs one address.
Everything about how a borrow behaves — the auto-deref, `->` reaching through, `T&` versus `ptr<T>` —
is in [Pointers & References](pointers_and_refs_v2.md), and none of it changes here.

The one thing to know is a lifetime rule you've already met: a borrow doesn't keep anything alive. Rule 2
of that chapter's "Lifetimes, and how to get hurt" applies to owning types verbatim — a borrow of a value
must not outlive the owner, and moving the owner elsewhere ends the borrow just as decisively as
destroying it.

And watch the overload set. Argument fitting ranks an exact match above a borrow, so if both
`total(array<int32> $items)` and `total(array<int32>& $items)` exist, the **copying** one wins the call.
Passing by borrow is a discipline you follow at the declaration; adding a borrow overload alongside a
by-value one doesn't enforce it, it just hides it.

## Temporaries, and how long they live

A call can hand you a value nobody stored, and you can use it. Reading a member off one —
`$o->make()->tag` — gives that value storage for exactly as long as the expression reading it, then
destroys it: once, after the read, and once *per evaluation* if the expression is a loop condition.
Nothing is different about what a temporary owes, only about how long it lives. The drop is the same drop
a local gets.

Calling a method on one works too, which is the same rule from the other side. A method takes the address
of the value it's called on, so `$o->make()->size()` needs storage for as long as the call — and gets it.
So does a member handed to a borrow parameter: `use($o->make()->handle)` lets the callee read through the
address, and the temporary is destroyed once the call returns.

Mutating one is legal and does nothing. `$o->make()->bump(1)` bumps the copy, which is then thrown away —
exactly as in any other language with temporaries.

What's refused is keeping something *past* the expression, because no lifetime a temporary can have would
cover it:

```echo
int32& $r = &$o->make()->x;    // error: the borrow would outlive the value
```

The same goes for that address through an assignment or a `return`, and for reading a *pointer* field off
a temporary — the address would outlive the bytes it points at. And `$o->make()->x = 5` is refused for a
different reason: the write isn't unsafe, it's simply lost, because nothing will ever read the bytes it
went into.

Bind the value to a variable first and all of them are ordinary code.

## Destruction

A type that owns something declares what to do about it:

```echo
struct Buffer
{
    ptr<uint8> $data;
    usize $size;

    destructor()
    {
        mem::free($this->data);
    }
}
```

A `destructor` takes no parameters, returns nothing, and you never call it. It runs when the owner's
scope ends, and it runs once — which is the whole reason moves exist. A struct that contains an owner
gets a destructor implicitly, and that implicit one destroys each field in reverse declaration order.

Three things follow, and all three are worth stating:

1. **A moved-from value isn't destroyed.** Otherwise `$b = mv $a` would free the buffer at the end of
   `$a`'s scope and leave `$b` holding it.
2. **A returned local is moved, not copied.** Otherwise every function that builds a `Buffer` and
   returns it would free the buffer on the way out.
3. **A destructor cannot fail and cannot be skipped.** There's no way to suppress one. If you need the
   resource released earlier, release it earlier, and make the destructor tolerate having nothing to do —
   as the one above does, since freeing `null` is a no-op.

## Copying deeply

When a type owns a resource, copying it has to duplicate the resource. Otherwise both copies point at one
buffer, and both destructors free it.

The compiler can't work out what that duplication is, and the reason is worth seeing. Ownership always
*ends* at a raw pointer — a `ptr<uint8>` field is where every owning type actually holds its resource —
and a raw pointer owns nothing as far as the type system can tell. So a member-wise copy would copy the
address and leave two owners of one allocation. The recursion has no bottom.

The type holding the pointer is the one that knows, and it says so by writing a constructor that takes a
borrow of itself:

```echo
struct Buffer {
    usize $length;
    ptr<uint8> $data;

    constructor(usize $length) {
        $this->length = $length;
        $this->data:$ = mem::alloc<uint8>($length);
    }

    // the copy constructor. no new keyword, no special form - a constructor whose single
    // parameter is a borrow of its own type *is* the copy, and that's how it's recognized
    constructor(Buffer& $other) {
        $this->length = $other->length;
        $this->data:$ = mem::alloc<uint8>($other->length);
        // ...and copy the bytes across
    }

    destructor() {
        mem::free($this->data);
    }
}

$a = Buffer(64);
$b = $a;          // calls it. two buffers, two owners, two frees
$c = Buffer($a);  // the same declaration, written out
```

`const Buffer& $other` works too, and is the more honest spelling — a copy reads its source.

Two things follow from recognizing a constructor rather than adding a spelling for copies.

There's exactly one way to copy a value: `$b = $a` and `Buffer($a)` resolve to the same declaration, so
they can't drift apart. And the recursion bottoms out, because the body is written rather than derived —
a type whose field owns something says what copying that field means:

```echo
struct Document {
    Buffer $body;

    constructor(Document& $other) {
        $this->body = $other->body;   // Buffer's own copy constructor answers for this
    }
}
```

A copy constructor is honored whether or not the type owns anything. `Point` needs none, and copying it
is still a copy of bytes with nothing inserted — but if `Point` declares one, that's what a copy of a
`Point` means. The alternative would have made `$b = $a` depend on whether the type happens to declare a
destructor.

Without one, an owning value is still moved or borrowed, never copied:

```echo
$a = array<int32>();
$b = $a;          // error: 'array<int32>' owns a resource and cannot be copied
                  //        implicitly. Write 'mv $a' to transfer ownership, take a
                  //        borrow ('array<int32>&') if the value is only being read,
                  //        or give 'array<int32>' a copy constructor.
```

That's not a lesser state to be in. `mv` is cheaper than any copy and says at the call site that the
value changed hands, and a type with no sound duplication — a unique handle, a lock — is right to have no
copy constructor at all.

### The one copy the compiler writes for you

There's a single case where nothing has to be written, because nothing has to be guessed. A struct whose
owning properties are all classes has exactly one thing a copy of it could mean: one more reference to
each.

```echo
class Handle { int32 $id; }

struct Pair {
    Handle $a;
    Handle $b;
}

$p = Pair(Handle(1), Handle(2));
$q = $p;          // fine. both handles retained, both released when $q goes away
```

The recursion has a bottom here that the general case doesn't: a class field is answered *at* the class,
without descending into what it holds. It reaches through struct-typed properties too, so a struct
holding a struct holding a class copies as well.

The split is worth stating, because it isn't "we cannot copy owners" — it's "we cannot copy owners we
have no rule for":

```echo
struct Pool {
    Handle $h;
    destructor() { /* ... */ }
}

$b = $a;          // error: 'Pool' owns a resource and cannot be copied implicitly
```

A declared destructor stops it. What a second value running that body would mean is exactly the question
its author hasn't answered — the pool might be counting what it has open, or closing something the copy
would close twice. Same for a property that owns a raw pointer: ownership ends there, and the type
holding it is still the only thing that knows.

One thing the synthesized copy is not: a name. `$q = $p` is the spelling; there's no `Pair($p)` to call,
because the compiler decides this after your file has been read and every written call in it has already
been resolved. A type you want to copy explicitly is a type you should write the constructor for — and if
you do, yours is used and nothing is synthesized.

### What replacing a value does

An assignment over a value that owns something destroys the old value, and it does so *after* the
right-hand side has been evaluated:

```echo
$a = Buffer(64);
$a = $a;              // copy from the live $a, then destroy the old one, then store
$a = shrink($a);      // the same window: shrink still reads a live value
```

There's only one correct order here, and it isn't the obvious one. Destroying first would hand the
right-hand side a value that has already been freed. So `$a = $a` isn't a special case worth checking
for — it's the visible tip of a rule that applies to every replacement.

## What a move cannot take

`mv` takes a whole place. A field or an element is refused:

```echo
$x = mv $doc->body;    // error: 'mv' can only move a whole variable - moving a field or an
                       //        element out of a value is not supported yet
$y = mv $items[0];     // same
```

The reason is that a partly-moved struct has to answer what its destructor does, and an element adds a
runtime index on top — there'd be nothing static to mark unset. Move the whole value, or take the field
by borrow.

For the same family of reasons, replacing a field that owns something is refused outside a constructor:

```echo
$doc->body = Buffer(64);   // refused when $doc->body owns a resource
```

The old field value's teardown is the same unanswered question as the partial move. Inside a constructor
it's fine, because there's no old value to tear down — which is also why initializing the same owning
field *twice* in one constructor is refused: the second write would leak what the first one built.

## Classes are the same rules, resolving differently

A `class` isn't an exception to this chapter. Everything above asks two questions of a value: does
copying it need a constructor, and who destroys it. A class answers the first with a retain and the
second with a release, and it moves the payload's teardown to the moment the count reaches zero, where
the compiler writes the deinit.

That's one type rule inside the same machinery, not a second mechanism. Which is why weak references —
the one thing counting can't handle on its own — get a section rather than a chapter.

## Weak references and cycles

Counting references has one hole, and it's worth seeing before the fix:

```echo
class Node
{
    int32 $tag;
    Node? $next;
    Node? $prev;

    destructor() { echo $this->tag; }
}

$a = Node(1, null, null);
$b = Node(2, null, null);

$a->next = $b;             // $b now has two owners: the local, and $a
$b->prev = $a;             // and so does $a
```

Neither destructor ever runs. Each object holds the other, so neither count can reach zero, and the two
blocks live until the process ends with nothing to warn you. That's not a bug in the counting — it's
what counting *is*, in every language that does it this way.

The way out is to say that one of the two edges doesn't own:

```echo
class Node
{
    int32 $tag;
    Node? $next;
    weak<Node> $prev;      // a back edge that keeps nothing alive
}
```

Now `$a` has one owner, the scope ends, both objects are destroyed, and both destructors run. One `weak`
on the back edge is the whole difference between those two programs.

### Taking one

`&$obj` on a class gives you a weak reference wherever a weak reference is what's being asked for — a
declared local, a field, a parameter, a return type:

```echo
weak<Node> $w = &$node;
$child->parent = &$node;
```

Where nothing is asking, `weak(...)` says it outright, which is also the spelling for an inferred
declaration:

```echo
$w = weak($node);
```

`weak<T>` requires `T` to be a class. Nothing else is counted, so nothing else has a count to opt out of.

### Reading one

You can't. That's the point — the object may be gone, and a weak reference that could be read would be
a dangling pointer with better manners. Upgrade it instead:

```echo
Node? $up = strong($w);
```

The result is [nullable](nullability.md), because the upgrade genuinely may fail. And `guard`, `??` and
`?->` all accept a `weak<T>` directly and do the upgrade for you:

```echo
guard Node $n = $w else { return; }
echo $w?->tag ?? -1;
```

### What it costs, and when the object dies

A class block carries two counts. The strong one is how many owners the *object* has; at zero its
destructor runs and its payload is torn down. The weak one is how many handles need the *block* to stay
readable, and only at zero is the memory given back.

So a weak reference outliving its object is an ordinary, safe situation rather than a bug. The object is
gone, the block is still there because you're holding it, and `strong` reads the strong count and answers
`null`. That gap between "the object died" and "the memory was freed" is the whole reason the second
count exists.

All the strong references together hold one weak count between them, so a freshly built object reads 1,
and each `weak<T>` naming it adds one more. `mem::ref_count` and `mem::weak_count` will tell you both if
you want to watch it happen.

A weak reference is an owner in this chapter's sense — of the block's readability, not of the object. It's
released at scope exit, copying one takes another, and a class holding one gets a deinit for it. None of
that needed a new rule: it's a retain and a release, the same pair a class field gets, with the type
deciding which of the two counts moves.
