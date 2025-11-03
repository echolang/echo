# Value Types

Echo has the scalar types you'd expect — `int32`, `float32`, `bool` — and it also lets you build your own
compound types out of them. Both are **value types**, which means one thing above all else: assigning one
copies it.

```echo
$a = 1;
$b = $a;      // $b is a copy of $a
$b = 2;
echo $a;      // 1
```

No surprises there. The interesting part is that your own types behave the same way.

## Structs

A compound value type is a `struct`:

```echo
struct Point
{
    float32 $x;
    float32 $y;
}
```

If you're coming from PHP this looks like a class with the interesting parts removed, but it isn't a class at
all — Echo has those too, and they behave completely differently. A struct is a value:

- It lives where you put it. A local lives on the stack and is destroyed when it goes out of scope. No
  allocation, no reference count, nothing to free.
- It's copied when you assign it or pass it, exactly like an `int32`.
- It can be **moved** instead, which is cheap and which the next section is about.

There's no `new`. You call the type:

```echo
$a = Point(1.0, 2.0);
$b = $a;        // a copy
$b->x = 3.0;    // $a is still (1.0, 2.0)
```

Arguments are positional. A struct that declares no constructor gets one taking its properties in declaration
order, which is where `Point(1.0, 2.0)` comes from.

The copy applies to arguments too, which is what makes this safe to write:

```echo
function squared(Point $p) : Point
{
    $p->x = $p->x * $p->x;
    $p->y = $p->y * $p->y;
    return $p;      // the caller's Point was never touched
}

$squared = squared($a);
```

`$p` is the function's own copy. Scribbling on it is fine, and the caller's `Point` doesn't change.

Note: `echo` prints one scalar, so `echo $a;` on a struct won't compile. Use `dprint($a);` when you want to see
the whole thing — it prints the type and every property:

```echo
dprint($a);
// [Point] {
//   [float32] $x = 1
//   [float32] $y = 2
// }
```

## Ownership and moving

A value type has exactly one owner. When the owner goes out of scope, the value is destroyed. That ownership
can be handed to somebody else, and handing it over is called a **move**:

```echo
$a = Point(1.0, 2.0);
$b = mv $a;      // $b owns it now. $a is unset
```

On a `Point` that's a silly thing to do — copying two floats is free, so a move saves nothing. It matters as
soon as a value owns a *resource*, like a heap buffer, because then a copy has to duplicate the buffer and a
move doesn't.

A function can ask for ownership by writing `mv` on the parameter:

```echo
function normalize(mv Point $p) : Point
{
    $length = std::math::sqrt($p->x * $p->x + $p->y * $p->y);
    $p->x = $p->x / $length;
    $p->y = $p->y / $length;
    return $p;
}
```

Here's the part I like: the call site has to say so too.

```echo
$a = Point(1.0, 2.0);

$normalized = normalize($a);
// error: '$p' takes ownership of this argument - write 'mv' in front of it, or the value
//        would be handed over without the call site saying so.

$normalized = normalize(mv $a);   // this is fine
echo $a->x;                       // error: '$a' has been moved out of.
```

A function signature can't quietly consume something you still thought you had. Every place a value stops
being yours is spelled `mv`, in your own source, and reading a moved-from variable is a compile error rather
than a surprise at runtime.

That's the summary. [Ownership & Moving](ownership_and_moving.md) is the real chapter: what happens when a
struct owns a resource, when a copy needs a constructor, and how destruction is ordered.

## And then there are classes

The other half of that chapter is the `class`, which is the same declaration syntax and the opposite of
everything above: many owners rather than one, counted, allocated on the heap, and destroyed when the last
owner goes away.

Worth knowing early, because it has one sharp edge: two classes referring to each other keep each other alive
forever. Breaking that needs one of the edges to be a `weak<T>` — see
[Weak references and cycles](ownership_and_moving.md#weak-references-and-cycles).
