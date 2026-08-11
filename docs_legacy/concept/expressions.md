# Expressions

Echo is statically typed, but you rarely have to say so. Most of this chapter is about how the compiler works
out what type an expression has, and the one rule that decides it when there's a choice: **the destination
wins.**

## Literals

A literal is a fixed value written directly in the code, and it's what gives a value its initial type.

```echo
$someInt = 1;        // int32
$someFloat = 1.0;    // float64
```

That second one catches people out, so it's worth saying plainly: **a bare decimal literal is a `float64`.**
Add an `f` when you want single precision:

```echo
$single = 1.0f;      // float32
```

Hex literals work too, and they take the smallest unsigned type that fits:

```echo
dprint(0xFF);         // [uint8] 255
dprint(0xFFFF);       // [uint16] 65535
dprint(0xFFFFFFFF);   // [uint32] 4294967295
```

`f` is the only suffix. There's no `420i32` or `1u64` — to get a specific integer type, say it at the
destination and the literal converts on the way in:

```echo
int64 $big = 420;    // int64
uint8 $small = 200;  // uint8
```

`int` and `float` are accepted as aliases for `int32` and `float32`, and `double` for `float64`. The explicit
spellings are what the rest of these chapters use.

### Losing precision is an error

A literal converts to whatever the destination asks for, as long as nothing is lost:

```echo
int64 $a = 1.0;      // works — 1.0 is exactly 1
int64 $b = 1.1;      // error: this would lose the .1
```

The same check catches a decimal that doesn't fit in single precision, which is the other half of why `f`
exists:

```echo
float32 $x = 3.14;   // error: the literal '3.14' is stored in 32bit float which will result
                     //        in the effective value 3.14
float32 $y = 3.14f;  // works
float64 $z = 3.14;   // works
```

Note: this check applies to *literals*. The compiler knows the exact value at compile time, so it can tell you.
It can't do that for a variable, and it doesn't pretend to — see the underflow example at the end.

Literals are converted at compile time, not at runtime. `int64 $a = 1.0;` costs nothing.

## Implicit type conversion

Echo makes assumptions about the type of a value based on where it's used.

I understand this is controversial to some. I still prefer it, because it greatly simplifies both the language
and the experience of writing it. The cost is some room for surprise — but the conversions aren't
semi-random, they follow a strict set of rules, and here they are.

### Floating point wins

Any arithmetic involving a floating point value produces a float of the same type.

```echo
dprint(3.14f * 2);   // [float32] 6.28

// conceptually
dprint(3.14f * 2.0f);
```

And remember that without the `f`, you're working in double precision:

```echo
dprint(3.14 * 2);    // [float64] 6.28
```

### Higher precision wins

When two floats meet, the wider one decides — the point being to not throw away bits you had:

```echo
dprint(3.14f * 2.0);   // [float64] 6.28
```

This applies to every arithmetic infix operator, and it applies left to right through the whole expression:

```echo
dprint(3.14f * 2 + 10);   // [float32] 16.28

// conceptually
dprint((3.14f * 2.0f) + 10.0f);
```

### The destination has the final say

If the context expects a specific type, the expression is converted to it. Depending on what's in the
expression, that happens at compile time or at runtime.

```echo
int32 $myNumber = 1.0;   // int32 1, decided at compile time
```

`1.0` converts to an `int32` losslessly, so the compiler just does it. Now put a variable in there:

```echo
$multiplier = 2;
float32 $val = 3.14f * $multiplier;   // float32 6.28

// conceptually
float32 $val = 3.14f * float32($multiplier);
```

`$multiplier` is a variable, so the compiler can't change its type for the sake of this one operation. A
runtime conversion is inserted instead. *An optimizer might well fold this away, but for the sake of the
example assume it doesn't.*

This is what "the destination wins" means in practice — the expected type reaches back into the whole
expression:

```echo
$i = 1;
$f = 1.0;

dprint($i + $f);           // [float64] 2   — no destination, so floating point wins
int32 $sum = $i + $f;      // [int32] 2     — the destination said int32
```

Same expression, two answers, and neither of them is a coin flip.

### When it can't be done

If the types are genuinely incompatible, you get a compile-time error:

```echo
uint8 $myVal = 10 + -1;   // error: -1 cannot be converted to uint8
```

Here's the catch, and it's the important one in this chapter: **that only works because they're literals.** The
compiler evaluated `10 + -1` and looked at the result. For variables, it can't:

```echo
uint8 $a = 10;
uint8 $b = 1;

$val = $b - $a;    // uint8 247. underflowed, silently
```

That's valid code and nothing warns you. The arithmetic is unsigned, `1 - 10` wraps, and you get 247. If you
want the negative number, use a type that can hold one.
