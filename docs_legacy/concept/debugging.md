# Debugging

Build with `-g` and the program can be stepped through in a debugger, one Echo line at a time.

```bash
echoc build -g -o app app.eco
lldb ./app
```

```
(lldb) breakpoint set --file app.eco --line 9
Breakpoint 1: where = app`_scaleZZMRBMLC5PointZMLPi + 20 at app.eco:9:5

(lldb) run
* frame #0: app`_scaleZZMRBMLC5PointZMLPi(p=0x000000016fdfdfa8, by=10) at app.eco:9:5
   8       int32 $sum = $p->x * $by;
-> 9       return $sum + $p->y;

(lldb) bt
  * frame #0: app`_scaleZZMRBMLC5PointZMLPi(p=..., by=10) at app.eco:9:5
    frame #1: app`main at app.eco:13:7

(lldb) frame variable
(Point *) p = 0x000000016fdfdfa8
(int) by = 10
(int) sum = 30

(lldb) p *p
(Point) {
  x = 3
  y = 4
}
```

Breakpoints resolve by `.eco` file and line, `step` and `next` move a statement at a time, the
backtrace names Echo functions, and locals, parameters and struct members are readable.

## Names have no `$`

Write `p count`, not `p $count`. A leading `$` is how lldb spells its own convenience variables, so a
debugger could never reach a name that had one. Everything else is spelled the way you wrote it.

## `-g` says nothing about which checks the program carries

That's `--debug` and `--release`, and the two are deliberately separate. A release build with `-g` is
a program with its `assert`s dropped that you can still step through — which is usually exactly what
you want when something only misbehaves in release.

```bash
echoc build --release -g -o app app.eco   # no asserts, still debuggable
```

## `-g` turns the optimizer off, unless you ask for it

On its own, `-g` also implies `--optimize none`. An optimized build reorders, folds and inlines until
stepping walks a program nobody wrote and most locals read `<optimized out>`, so a `-g` build that
still did that would be debug information in name only.

If you need to debug the optimized program — a bug that only appears at `--optimize whole`, say — ask for it and
both apply:

```bash
echoc build -g --optimize whole -o app app.eco
```

The debugger will tell you when a value has been optimized away rather than showing you a stale one.

## What it costs, and what it invalidates

Debug information lives in the object, so a `-g` build is a different build: it has its own module
cache entries, and `-g` and non-`-g` artifacts coexist rather than overwrite each other, exactly as
`--debug` and `--release` do. Switching the flag recompiles; switching back reuses what was already
there.

The program itself is unchanged. Nothing in `-g` alters what the code does — only what the object says
about itself.

## Only `echoc build`

`echoc run` compiles in memory and hands the result straight to a JIT, which writes no object file for
a debugger to open. It accepts `-g` and tells you it can't honor it. Use `build`.

## Containers print as their contents

The standard library's types read the way you wrote them:

```
(lldb) frame variable
(array<string>) names = ["Alice", "Bob"]
(array<int32>)  nums  = [1, 2, 3]
(map<string,int32>) ages = {"Alice": 30, "Bob": 41}
(string) s     = "Alice"
(string) part  = "Ali"
(int32?) maybe = 7
(int32?) missing = null
(Node *) n = 0x000000014e804080 Node(id = 9)

(lldb) v names[1]
(string) names[1] = "Bob"

(lldb) v -P1 n
(Node *) n = Node(id = 9) {
  id = 9
  [refcount] = 1
}
```

A class shows the fields you declared; its reference counts are gathered under `[refcount]` rather
than sitting in front of them. A long container is previewed and counted — `[1, 2, 3, 4, ...] (1000
items)` — and expanding it still lists every element.

This is a **debugger-side** feature, not part of the object: the compiler describes layout, and how a
container reads is a fact about the standard library. It lives in `tools/echo_lldb.py`.

### Turning it on

In VSCode it's automatic — the `echo:` launch configurations import it. From a terminal, one line:

```
(lldb) command script import tools/echo_lldb.py
```

or permanently, by putting that line in `~/.lldbinit` with an absolute path. A repository-local
`.lldbinit` deliberately isn't shipped: lldb ignores one unless `target.load-cwd-lldbinit` is turned
on, and turning that on lets any directory you start a debugger in run code.

To see the raw layout instead — which is what you want when you suspect the library itself:

```
(lldb) type category disable echo
```

## Two things to know

**`v` subscripts, `p` does not.** `v names[1]` works; `p names[1]` reports *"type 'array<string>' does
not provide a subscript operator"*, because the expression evaluator parses as C++ and finds no
`operator[]`, while the variable path reads the formatted children. Use `v` (`frame variable`) for
elements.

**An uninitialized value says so.** Stop before a local's initializer has run and a container reads
`<uninitialized?>` and a string `<unreadable>` rather than showing you whatever was on the stack.

## What you will not see yet

A `mem::buffer<T>` shows a capacity and no elements — it has no length, so nothing in it says which
slots hold a live value, and any element shown would be invented.

A `weak<T>` shows the block it points at rather than the object, and an interface value shows its two
pointers. Neither has a formatter yet.

A `map` whose recorded length disagrees with its occupied slots renders what it actually found — the
count is not taken on trust.

The `echo: debug test.eco (cpptools)` fallback configuration gets **none** of this. It drives lldb
through `lldb-mi`, which has no way to load the formatters.
