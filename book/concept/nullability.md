# Nullability

PHP lets any variable hold `null`, and finding out is a runtime job. Echo asks the same question at
compile time, and the answer is in the type. That's the whole chapter: a type either promises there's
a value, or it says there might not be — and the compiler won't let you confuse the two.

You've already met the idea, in fact, without it being called this:

```echo
int32& $r = &$var;         // certainly points at something
ptr<int32> $p = null;      // might not
```

`T&` and `ptr<T>` are the same machine address and differ in exactly one respect — whether `null` is
allowed. That distinction used to live only on pointers. It now lives on every type, and it is spelled
with a `?`.

## The question mark

Write `?` after a type and the value may be absent:

```echo
int32  $count = 0;         // there is a number
int32? $maybe = null;      // there might be

Node  $node = Node(1);     // there is an object
Node? $tail = null;        // this is the end of the list
```

A value slides into a nullable without saying anything, because doing so only gives up a guarantee:

```echo
int32? $widened = 41;      // fine
```

Going the other way is where the compiler stops you, and it is the point of the whole feature:

```echo
int32? $maybe = lookup($key);
int32  $sure  = $maybe;    // error: cannot implicitly convert 'int32?' to 'int32'
```

There is nothing wrong with that program *except* that it has not said what happens when the value is
absent. The next three sections are the three ways to say it.

`null` is the empty value of every nullable, and there is only ever one kind of empty — `T??` is just `T?`. You
can ask directly:

```echo
if ($maybe == null) { echo "nothing"; }
```

There's one deliberate exception to all of this, and it's a pointer. A `ptr<T>` is nullable, and `->` still
reaches through it with no `?`:

```echo
ptr<Node> $p = find();
echo $p->tag;              // no '?' needed, and none accepted
```

That's the pointer auto-deref from [Pointers & References](pointers_and_refs_v2.md#nullability), where the rule
has always been to null-check the *address* rather than the value. It's a decision about pointers rather than
about nullability, which is why this chapter leaves it alone.

## Classes are not nullable any more

This is the change most likely to touch code you have already written. A class handle used to accept
`null` whether or not you wanted one:

```echo
Node $n = null;            // this used to compile
```

It no longer does. A bare `Node` is a promise that there is an object, and a field or local that may
hold nothing says so:

```echo
class Node
{
    int32 $tag;
    Node? $next;           // a list ends, and the type is where that gets said
}
```

The gain is worth the edit. Reaching through a `Node` needs no thought, because there is always
something there; reaching through a `Node?` needs one of the three forms below, and the compiler tells
you which line needs it.

## `guard` — bind it, or leave

When the rest of the block has no meaning without the value — which is most of the time — bind it and
get out otherwise:

```echo
function greet(int32 $key) : int32
{
    guard int32 $v = lookup($key) else { return -1; }

    return $v + 1;         // $v is an int32 from here on, read plainly
}
```

`$v` is declared in the *enclosing* scope with the non-null type, so everything after the guard reads it
without ceremony. That is what makes `guard` the form to reach for first: it is the one that gets the
absence out of the way instead of carrying it forward.

The `else` block must leave — `return` or `die`. It is not a style rule:

```echo
guard int32 $v = lookup($key) else { echo "missing"; }   // error
echo $v;                                                 // ...because of this line
```

If the else arm could fall through, `$v` would be in scope on a path where the value was never there. The compiler
refuses the arm rather than the read.

**`guard` is how you narrow a nullable — an `if` doesn't.** This is the one place a PHP habit will bite you:

```echo
if ($maybe != null) {
    echo $maybe->tag;      // still an error: $maybe is an int32? in here too
}
```

Testing a nullable tells you something; it doesn't change the type. `guard` changes the type, and it changes it
for the whole rest of the scope rather than for one block — which is why it's the form to reach for first.

## `??` — supply a replacement

When absence has a sensible stand-in:

```echo
echo lookup($key) ?? 0;
echo find($id) ?? Node(0, null);
```

The right side runs **only** when the left is absent, so it may be as expensive as it likes. Fallbacks
chain, each tried in turn:

```echo
echo lookup($a) ?? lookup($b) ?? 0;
```

## `?->` — skip the work

When absence just means there is nothing to do:

```echo
echo $maybeNode?->tag ?? -1;
```

Everything after the `?->` is inside the one short circuit, so an absent base skips the whole rest of the chain —
you write the `?` at the link that may be absent, not at every link after it:

```echo
echo $node->next?->next?->tag ?? 0;
```

Each `?` there is a link that genuinely may be missing. The compiler says so if you write one that
cannot be:

```echo
echo $node?->tag;          // error: '?->' needs a value that may be absent - write '->'
```

The result of a `?->` is nullable, because the whole expression is absent whenever the base was — which
is why it pairs so naturally with `??`. A chain ending in a `void` call stays `void`: there is nothing
for a statement to be absent.

## What it costs

Nothing, for most types. A pointer, a class handle and a weak reference already have a null value of
their own, so `Node?` is the same single machine word `Node` is, and the difference exists only in the
type system.

Everything else — a primitive, a struct, an interface, a callable — has no spare bit pattern to mean
"absent" with, so `T?` is a tagged pair, one byte plus the value. `int32?` costs eight bytes where
`int32` costs four.

You never see either shape. `guard`, `??`, `?->` and `== null` all read whichever one applies, and no
program can tell them apart.

## Weak references

The other way a value can turn out not to be there is a
[weak reference](ownership_and_moving.md#weak-references-and-cycles) — a handle on an object that does
not keep it alive. Upgrading one gives you a nullable, because the object may be gone:

```echo
weak<Node> $w = &$node;

Node? $upgraded = strong($w);
```

All three forms accept a `weak<T>` directly and do that upgrade for you, so the reading is the same as
for anything else:

```echo
guard Node $n = $w else { return; }
echo $w?->tag ?? -1;

Node $picked = $w ?? Node(0, null);
echo $picked->tag;
```
