# Operators

Arithmetic, logical, comparison — the usual suspects, and they work the way you'd expect:

```echo
$a = 1 + 2; // 3
```

That's the boring half. The interesting half is that operators in Echo are just functions, which means you can
overload them for your own types, and you can invent entirely new ones.

## Bitwise operators

`&` `|` `^` `<<` `>>`, on integers only. A float, a bool or a pointer beside one is an error naming both
types — a bool is a yes/no rather than a one-bit number, so `$flag << 1` is refused rather than reinterpreted.

```echo
int32 $flags = 12;

echo $flags & 10;   // 8
echo $flags | 10;   // 14
echo $flags ^ 10;   // 6
echo $flags << 2;   // 48
echo $flags >> 2;   // 3
```

**`>>` follows the operand's signedness.** Over a signed integer it keeps the sign bit, over an unsigned one
it brings in zeroes — so the same bit pattern shifts to two different answers depending on the type it is
held in, which is what you want and is worth seeing once:

```echo
int32 $negative = -16;
echo $negative >> 2;      // -4, the sign bit is replicated

uint32 $same_bits = 4294967280;
echo $same_bits >> 2;     // 1073741820, zeroes come in
```

**The count is a count, not a second operand.** Every other binary operator meets its two sides at one
type — `int32 + int64` is an `int64` addition — but a shift is performed at the *left* operand's type
whatever the count happens to be declared as, and only the value being shifted has a say in the width or
the signedness:

```echo
int32 $negative = -16;
uint32 $count = 2;

echo $negative >> $count; // still -4. an unsigned count does not make it an unsigned shift

uint8 $byte = 200;
int64 $three = 3;

echo $byte << $three;     // 64 - a uint8 shift that wrapped, not an int64 one that did not
```

All five fold at compile time, so they work inside `const if` and `const(...)`. A shift count at or above
the operand's width is refused rather than answered — at runtime that shift is undefined, so there is no
answer to agree with, and it is refused the same way whether or not you asked for the fold:

```echo
int32 $x = 1 << 32;        // error: shifts a 'int32' by 32 or more bits
int32 $y = const(1 << 32); // the same error
```

> **Watch the space before `&`.** `&$x` means *the address of `$x`* and `& $x` means the bitwise operator,
> because `&` glued to what follows it lexes as an address-of. So `$h & $mask` is a mask and `$h &$mask`
> is not — write spaces on both sides of an infix `&` and the question never comes up. This is the one
> place in the language where whitespace changes meaning, and it is the price of spelling both with `&`.

## Operator overloading

Echo lets you override operators for your own types. So yes, a `Point` can know how to add another `Point`,
because writing `addPoints($a, $b)` everywhere gets old pretty quickly.

```echo
struct Point {
    float64 $x;
    float64 $y;
}

operator (Point $a) + (Point $b): Point {
    return Point($a->x + $b->x, $a->y + $b->y);
}

operator (Point $a) + (int32 $b): Point {
    return Point($a->x + $b, $a->y + $b);
}

$pointA = Point(1.0, 1.0);
$pointB = Point(2.0, 2.0);

$pointC = $pointA + $pointB;

$pointD = $pointA + 2;
```

An operator is a **function**, and everything you already know about functions applies to it. A symbol
names an overload set the same way a name does, resolution picks between overloads by the same rules,
and a use site no declaration answers gets the same diagnostic a call would:

```
No overload of 'operator +' accepts these arguments. Candidates are:
  operator +(A, A)
  operator +(A, int32)
```

Those candidates are listed because one of them mentions a type you wrote. Where none of them does,
naming them would be noise — every operator in the program shares one namespace, so the set behind a
symbol includes declarations from libraries you never asked about. Then the message is about your
operands instead:

```
operator '==' is not supported on operands of type 'P' and 'P'
```

Two consequences worth knowing:

- **An operator is declared at file scope, not inside a type.** It is not a member of either of its
  operand types — `Point + Distance` belongs to neither — and its symbol is visible to the whole
  program, so it cannot be scoped to a struct body or a block.
- **You can declare an operator anywhere and use it anywhere.** Above its declaration, below it, in
  another file; the types it mentions may themselves be declared further down. Declaration order never
  matters.

You can only overload a built-in operator for your **own** types. At least one operand has to be a type you
declared — an `operator (int32 $a) + (int32 $b)` would never be used, because `+` over two
integers already means something, so it is refused where it is written rather than silently ignored.

## Custom operators

And now the part that's a bit different: you can define entirely new operators, not just
overload the existing ones.

```echo
// returns the average of two numbers
operator (float64 $a) avg (float64 $b): float64 {
    return ($a + $b) / 2.0;
}

$percentage = 10.0 avg 20.0; // 15.0
```

A symbol may be a word, like `avg`, or punctuation, like `<=>` or `!!`. A word operator does not stop
being an ordinary name — you can still declare and call a `function avg(...)` alongside it.

### Precedence

Every operator has a precedence deciding what binds to what. **A smaller number binds tighter.** These are the
built-in tiers:

| Precedence | Operators | Associativity |
|---|---|---|
| 20 | `++` `--` | right |
| 30 | `**` | right |
| 40 | `*` `/` `%` | left |
| 50 | `+` `-` | left |
| 60 | `<<` `>>` | left |
| 70 | `&` | left |
| 80 | `^` | left |
| 90 | `\|` | left |
| 100 | `<` `>` `<=` `>=` `==` `!=` | left |
| 110 | `&&` | left |
| 120 | `\|\|` | left |
| 130 | `=` | right |

A custom operator declares its precedence on that same scale, so you can place it between two built-in
tiers:

```echo
// binds tighter than `*`, so `2 avg 3 * 4` means `(2 avg 3) * 4`
operator(35, left) (float64 $a) avg (float64 $b): float64 {
    return ($a + $b) / 2.0;
}
```

Declare it once. A symbol binds one way everywhere in a program — two declarations of `avg` with
different precedences are an error, because otherwise two files would read the same expression
differently. A built-in symbol keeps the language's precedence, so an overload of `+` cannot declare
one.

**Without a clause a custom operator gets precedence 95**: looser than every arithmetic and bitwise
operator, tighter than every comparison. So `1 + 2 avg 3 + 5` groups its operands first, as
`(1 + 2) avg (3 + 5)`, and `$a avg $b > 3` compares the answer.

### Unary operators

These take a single operand and can be prefix or suffix.

You don't declare a precedence for one. A unary operator and its operand are always taken together before any
binary operator gets a look, so there's no number that would change anything.

With unary operators it does matter whether the operator is prefix or suffix, and the two are separate
declarations even for the same symbol:

```echo
struct Flag { bool $on; }

// prefix
operator !!(Flag $f): bool {
    return $f->on;
}

// suffix
operator (Flag $f)?: bool {
    return $f->on;
}

$f = Flag(true);
echo !!$f;
echo $f?;
```

One symbol can be both infix and prefix — `-` is — but not both infix and suffix, because `$a blah $b`
would not say whether the symbol closes the left operand or opens the right one.

`++` and `--` cannot be declared as suffix operators: `$i++` is a statement in Echo, and it always
means `$i = $i + 1`. `=` cannot be declared either, for the same reason — assignment is a statement,
not an expression.

#### Example: a string handle

Passing an integer handle around is efficient and thoroughly unpleasant to write. A suffix operator can hash
the string literal for you and hand back the handle.

```echo
operator (string $s)_handle: usize {
    $hash = 5381;
    $i = 0;
    while ($i < $s->size()) {
        $hash = $hash * 33 + $s->byte_at($i);
        $i = $i + 1;
    }
    return $hash;
}

$handle = "my.resource.texture.albedo"_handle;
```

A suffix operator applies directly to a literal, with no separator. If all values are known at compile
time the hash could in principle be folded to a constant — that is an optimization Echo does not do
today, so the loop above runs at runtime.

### Example: custom units

This is my favorite use for it. Custom units, so an expression reads like the thing it's actually computing.

```echo
struct Distance {
    uint64 $millimeters;
}

operator (Distance $a) + (Distance $b): Distance {
    return Distance($a->millimeters + $b->millimeters);
}

operator (uint64 $a)mm: Distance { return Distance($a); }
operator (uint64 $a)cm: Distance { return Distance($a * 10); }
operator (uint64 $a)m: Distance { return Distance($a * 1000); }

$distance = 1m + 50cm + 500mm; // 2000 millimeters
```

Note that the suffix operators here take a primitive on both sides of the declaration. That is fine and
is the point of them — the "at least one of your own types" rule is about overloading a *built-in*
symbol, and `mm` is not one.

## Generic operators

An operator over a generic type needs somewhere to say where its type parameter comes from, and it goes where a
function's does — right after the thing naming the declaration, which for an operator is the `operator` keyword
itself:

```echo
struct Vec<T> {
    T $x;
    T $y;
}

operator<T> (Vec<T> $a) + (Vec<T> $b): Vec<T> {
    return Vec<T>($a->x + $b->x, $a->y + $b->y);
}

$a = Vec<int32>(1, 2);
$b = Vec<int32>(3, 4);
$c = $a + $b;
```

The parameter is **inferred from the operands**, exactly as it is at any other call site — an
operator use site writes nothing but its operands, so there is no other way it could be decided.
That is also the one rule the declaration has to satisfy: **every type parameter has to be mentioned
by an operand**, or nothing could ever bind it, and it is refused where it is written rather than
failing at every use.

A precedence clause, if there is one, follows the list: `operator<T>(35, left) (Vec<T> $a) avg ...`.

An operator over a **concrete** instantiation is an ordinary declaration and needs no list:

```echo
operator (Vec<int32> $a) - (Vec<int32> $b): Vec<int32> { ... }
```

You can declare both. A concrete overload beats a generic one, which is the same rule that already
decides between a concrete function and a template.

## Indexing

`$a[$i]` is an operator too, and its declaration is the one shape where the second operand sits
**inside** the symbol:

```echo
struct Grid {
    ptr<float64> $cells;
    usize $cols;
}

operator (Grid& $g)[usize $row, usize $col]: float64& {
    return &$g->cells:$[$row * $g->cols + $col];
}

$g[1, 2] = 4.0;
echo $g[1, 2];
```

Five things about it are rules rather than details:

1. **The borrowing form returns a borrow**, `T&`, and nothing else. `$a[$i]` is a *place* — it reads,
   writes, takes `&` and chains with `->`, all through the address the operator hands back. An overload
   returning by value would make whether it is a place depend on which overload won.
2. **The container is the first operand**, and the indices follow in the order the brackets write
   them. Any number of them: `$m[$row, $col]` is a three-operand declaration, and `$map["key"]` is a
   two-operand one whose index is not a number at all.
3. **An empty `[]` is the append contract**, one operand, returning a borrow of the slot one past the
   end. It shares the `operator []` name with the element form and is told apart from it by arity
   alone. This is what `$a[] = $v` calls — see [Arrays](arrays.md), "Appending".
4. **No precedence.** `[` binds like `->`, tighter than every binary operator, and never reaches the
   precedence table — so there is no number to declare and writing one is an error.
5. **A `=` after the bracket declares the *write* contract**, which is a second, separate declaration —
   see below.

### Declaring the write

`Grid` above needs nothing more: `$g[1, 2] = 4.0` writes through the place its bracket handed back, which is
what every ordinary container wants. But a container that *seats* an element on demand — a map, where writing
a new key has to grow the table and writing an existing one has to destroy what was there — cannot hand back a
place at all. There is nothing valid in the slot to hand back.

So the write is declarable on its own:

```echo
operator<K, V> (map<K, V>& $m)[const K& $key] = (V $value): void {
    usize $at = $m->seat($key);

    mem::init<V>($m->values:$[$at], mv $value);
}
```

`$m[$key] = $value` is then **one call** — the value comes in as the third operand, and the body owns the
whole write. Three rules:

- **It returns `void`**, because it is a statement and not an expression. `$m[$k] = $v` produces no value,
  and this is the one operator in the language that is written that way.
- **It is a separate overload set** from the borrowing bracket, so a type declares both and the *position*
  chooses: an assignment picks the write, and reading, `&`, and `->` through it all pick the borrow. There is
  no precedence between them and no fit rule in between — the position decides, full stop.
- **A type that declares only the borrowing form is unaffected.** `$c[$i] = $v` is then the ordinary write
  through a place it always was, which is what `array<T>` is.

An empty `[]` before the `=` is the append write, `$c[] = $v` — told from the element write by arity, the same
way the two borrowing forms are.

`[` and `]` cannot appear in any other operator symbol, which is what keeps a bracket meaning exactly
one thing: ask this collection for one of its elements. Indexing a raw address is
`$p:$[$i]` and belongs to [Pointers & References](pointers_and_refs_v2.md).
