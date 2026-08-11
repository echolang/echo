# Constants

Every program has a few values that never change — a buffer size, a limit, a magic byte. PHP has `const`
and `define()` for those, and Echo has `const` too. It looks about how you'd expect:

```echo
const usize MAX_CAPACITY = 4096;

usize $room = MAX_CAPACITY;
```

The difference is what the name actually *is*. A constant has no storage anywhere in your program. It's
an expression the compiler copies into every place you use it — which is the whole chapter, and every
rule below falls out of it.

That's also why it's written with a **bare identifier and no `$`**: `$` is how this language spells a
value with storage, and a constant has none. Convention is `UPPER_SNAKE_CASE`; nothing enforces it.

## A constant is not a const variable

Both are spelled with `const`, and they are different things:

```echo
const usize $max = 100;      // a const *variable*: storage, in the scope it was written in
const usize MAX = 100;       // a *constant*: no storage, copied into every use site
```

A `const` variable is an ordinary local that cannot be written through. It lives wherever it was written —
including inside a block:

```echo
if ($ready) {
    const $attempts = 3;     // fine: a variable, local to this block
}
```

A constant can't, and that's a consequence rather than a restriction. Remember what a constant is: an
expression, copied to each place you use it. A block isn't somewhere a copy could be taken *from*. So you
declare a constant at file scope, in a namespace, or in a struct body, and nowhere else.

## What the copy means

There is no evaluator behind a constant. The compiler does not work out a number and hand that around — it
takes the expression you wrote and clones it into every reference:

```echo
const TICK = tick();

echo TICK;    // calls tick()
echo TICK;    // calls tick() again
```

That is deliberate, and it is what lets the initializer be *any* expression — a call, a constructor,
arithmetic over other constants — rather than the small closed set an evaluator could fold:

```echo
const usize PAGE = 4096;
const usize HALF_PAGE = PAGE / 2;      // a constant in terms of another
const ORIGIN = point(3, 4);            // a fresh point per use site
const GREETING = 'hello';
```

The cost is the flip side of the same coin: a call in a constant runs once per use site, and a constructor builds
a value per use site. If you want one value shared, that's a variable.

Why no evaluator, then? Because nothing in Echo actually needs one. There are no fixed-size arrays, no value
generics and no `case` labels — nowhere a folded number would answer something the copied expression doesn't. An
evaluator would have bought a smaller feature: it could only ever have accepted the arithmetic it knew how to
fold, and `const ORIGIN = point(3, 4);` would not be in that set.

Two shapes get refused at the declaration:

```echo
int32 $x = 5;
const BAD = $x + 1;                      // refused: `$x` does not exist where the copy lands
const HANDLER = function() { ... };      // refused: a closure captures *where it is written*
```

A cycle is refused too — substitution has no fixed point to stop at:

```echo
const A = B;
const B = A;                             // refused: 'A' is defined in terms of itself
```

## Where a constant can be written

**At file scope**, and in whatever namespace the file declares into:

```echo
namespace app;

const LIMIT = 7;

// `LIMIT` from inside app, `app::LIMIT` from outside - the same outward rule types follow
```

**In a struct or class body**, where it belongs to the type and is reached through it:

```echo
struct buffer
{
    const usize CAPACITY = 4096;

    usize $used = self::CAPACITY / 4;

    function headroom() : usize
    {
        return self::CAPACITY - $this->used;
    }
}

echo buffer::CAPACITY;
```

Inside the struct it's `self::CAPACITY`; from outside, `buffer::CAPACITY`. Never a bare `CAPACITY`.

That's the same line the language already draws for a nested type: `A::Inner(1)` resolves and a bare `Inner(1)`
doesn't. You reach a member through its owner, rather than by walking outward from wherever you happen to be.

You can't put a constant in a *generic* type. Its expression is copied into use sites before type arguments are
known, so an initializer mentioning the owner's `T` would have nothing to substitute it from.

## Constants and library modules

A constant is the one way to publish a named value from a library module. A file-scope *variable* in a
non-entry module is parsed, checked and then silently dropped — a module's file root emits only functions, and
a file-scope variable is a local of the implicit entry point nobody wrote. A constant needs no storage, so the
question does not arise:

```echo
// lib/src/limits.eco
namespace modlimits;

const usize PAGE = 4096;
```

```echo
// app.eco
usize $page = modlimits::PAGE;
```

The clone lands in the *consuming* module, which is also what keeps the library's object independent of who
consumes it — the invariant the module cache rests on.

The standard library is a library module like any other, and this is how it publishes `std::math::PI`,
`std::math::TAU`, `std::math::E` and the rest. Each one comes in two spellings — `PI` is a `float64` and
`PI_F32` a `float` — because a constant *is* its literal and a float literal is double precision unless it
ends in `f`, so the two are the ordinary overload-selecting types rather than a conversion:

```echo
echo std::math::cos(std::math::PI);       // the float64 overload
echo std::math::cos(std::math::PI_F32);   // the float one
```

## Typed and untyped

An untyped constant is its initializer, exactly as written, so `const MAX = 100;` hands each use site an
`int32` literal. Writing the type settles it at the declaration instead:

```echo
const MAX = 100;             // an int32 literal, at every use site
const usize MAX = 100;       // a usize, decided here
```

## A condition can never name a constant

```echo
#[if: MAX > 10]              // not possible, and not an omission
```

`#[if: ...]` is resolved by a **token filter that runs before the first parse pass** — it decides which regions
get parsed at all, which is what makes an excluded region free to name symbols this platform doesn't have. A
constant's name is published in a later pass. So at the moment a condition is evaluated, there's no Echo
declaration to read, and none can be made to. A condition sees the target and the command line, and nothing
else — see [Conditional compilation](conditional.md).

The two compose fine in the other direction, though. A constant inside an `#[if:]` region is dropped with the
rest of that region, never parsed and never published:

```echo
#[if: os == darwin]
const PLATFORM_LIMIT = 10;
#[elif: os == linux]
const PLATFORM_LIMIT = 20;
#[end]
```
