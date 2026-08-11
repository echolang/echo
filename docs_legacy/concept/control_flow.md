# Loops and branches

Three loops, and you already know two of them. `while` runs until its condition falls false, `foreach`
walks something you can iterate — that one has [a chapter of its own](iteration.md) — and `for` counts.

```echo
int32 $i = 0;
while ($i < 3) {
    echo $i;
    $i = $i + 1;
}

for (int32 $i = 0; $i < 3; $i++) {
    echo $i;
}

foreach ($items as $item) {
    echo $item;
}
```

Braces are always required. There is no one-line body, and no `do ... while` yet.

## `for`

```echo
for (int32 $i = 0; $i < 10; $i++) {
    echo $i;
}
```

Three clauses, in the order they happen: the initializer runs once, the condition is checked before every
turn, and the step runs after every turn. **All three are required** — `for (;;)` is an error rather than
an infinite loop, and each missing one gets told what to write instead:

```echo
for (; $i < 10; $i++) { }
// error: a 'for' needs all three clauses - this one has no initializer.
//        write 'for (int32 $i = 0; $i < 10; $i++)', or use 'while' for a loop that
//        initializes nothing.
```

That's on purpose. A `for` with no step *is* a `while`, and having two spellings of one loop is worth less
than having each spelling mean one thing.

### The variable belongs to the loop

```echo
for (int32 $i = 0; $i < 3; $i++) {
    echo $i;
}

echo $i;
// error: The variable '$i' is not declared in the current scope
```

`$i` lives from the initializer to the closing brace, and nowhere else — the same rule any block already
has, which is why there's no extra rule to remember. If you want the value afterwards, declare it
outside and let the initializer write it:

```echo
int32 $found = 0;
for ($found = 0; $found < $limit; $found++) {
    if (matches($found)) {
        break;
    }
}
echo $found;                  // whatever the loop left in it
```

The initializer takes either shape: a declaration, or an assignment to something that already exists.

### `continue` runs the step

This is the whole reason `for` exists as its own loop rather than as a shorthand:

```echo
for (int32 $i = 0; $i < 6; $i++) {
    if ($i == 3) {
        continue;
    }
    echo $i;                  // 0 1 2 4 5
}
```

`continue` jumps to the **step**, not to the condition. Write the same thing as a `while` with the
increment at the top of the body and that `continue` skips it, and the loop never ends. It's a real trap
in C-family languages and it's the one thing the language can spare you here.

`break` leaves the innermost loop, and nothing else changes about it — see below.

### What it costs

Nothing over the `while` you'd have written by hand. It's one condition block, one body, one step block,
and after optimization it's the loop you expect:

```echo
for (usize $i = 0; $i < $a->count(); $i++) {
    $sum = $sum + $a[$i];
}
```

compiles to a vectorized reduction at `--optimize whole`, the same as an index-free `foreach` does.

## `break` and `continue`

`break` leaves the innermost loop; `continue` starts its next turn. Both work in all three loops, and
both only mean something inside one:

```echo
break;
// error: 'break' is only meaningful inside a loop - there is no loop here to leave.
//        a closure or a nested function does not inherit the loop it was written in
```

There is no `break 2` and no labelled loop. An inner `break` leaves the inner loop only:

```echo
for (int32 $outer = 0; $outer < 3; $outer++) {
    for (int32 $inner = 0; $inner < 10; $inner++) {
        if ($inner == 2) {
            break;            // leaves the inner loop; the outer one steps and carries on
        }
        echo $inner;
    }
    echo "-";
}
```

Anything you own inside the loop is destroyed on the way out, whichever way you leave — falling out of the
condition, `break`ing, or `return`ing from inside the body. That's [ownership](ownership_and_moving.md)
doing its ordinary job; loops need no special case.

## Ranges

`for` counts, but most of the time you're counting *over* something, and there's a shorter way to say it:

```echo
foreach (0 .. 10 as $i) {
    echo $i;                  // 0, 1, ... 9
}
```

`..` builds a **range**, and a range is something you can loop over. `0 .. 10` stops *before* 10, which is
what makes it fit a count with nothing to adjust:

```echo
foreach (0 .. $a->count() as $i) {
    echo $a[$i];              // every index, no off-by-one to get wrong
}
```

When the upper bound is a real value rather than a length, `..=` includes it:

```echo
foreach (1 ..= 5 as $n) {
    echo $n;                  // 1, 2, 3, 4, 5
}
```

The spaces are optional — `0..10` is the same thing. A range going backwards or nowhere is simply empty,
which is the answer you want for an empty collection:

```echo
foreach (5 .. 5 as $x) { }    // never runs
foreach (10 .. 0 as $x) { }   // never runs, and is not an error
```

A range is an ordinary value. You can name one, pass one, and loop over it later:

```echo
range<int32> $window = 10 .. 20;
foreach ($window as $v) { ... }
```

And it costs what the hand-written loop costs — the cursor inlines away completely, so a range loop
vectorizes exactly as an indexed one does.

There is nothing special about `..` in the compiler, incidentally. It's a plain
[declared operator](operators.md) living in the standard library, returning a plain struct that declares
`contract::iterable` the same way [a type of your own would](iteration.md#writing-something-you-can-loop-over).
If you want `0 .. 10 by 2`, that's a change to a library file and not to the language.

## `if`, `guard`, and the compile-time branch

`if` / `else` is what you'd expect and needs no chapter. Two relatives are worth knowing about and live
elsewhere:

- **`guard`** unwraps something that may be absent and requires you to leave if it is —
  [Nullability](nullability.md).
- **`const if`** is a branch the *compiler* takes, so the arm that isn't chosen never reaches your program
  at all — [Conditional compilation](conditional.md).
