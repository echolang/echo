# `tests_eco/` — end-to-end tests

Each test is **two** files, in any nested subdirectory:

```
tests_eco/<anything>/<name>.eco    # the Echo program
tests_eco/<anything>/<name>.test   # everything asserted about it
```

The runner ([`tests/e2e_eco.cpp`](../tests/e2e_eco.cpp)) walks this tree recursively at **runtime**, so
adding, moving or nesting a pair never requires reconfiguring CMake. Either file without the other is
an error — a `.test` whose program was renamed away would otherwise assert nothing, invisibly.

## The `.test` format

A settings header, then delimited sections:

```
flags: --release
stdlib: off
expect: fail
mode: build

--- OUT --->
42
--- IR --->
CHECK: define i32 @main()
CHECK-NOT: call ptr @malloc
--- RAST --->
CHECK: Module: [main]
CHECK: ReleaseNode
```

Everything is optional except `--- OUT --->`. The common case is two lines:

```
--- OUT --->
12
```

**Anything the format does not understand is an error, never a no-op** — an unknown or repeated
setting, a value outside a setting's enumeration, a header line with no colon, an unknown or repeated
section, an empty directive section, a missing `OUT`. A `.test` that is quietly half-understood is a
test that quietly asserts less than its author wrote.

### Settings

| Key | Values | Meaning |
|---|---|---|
| `flags` | anything | extra `echoc` flags, spliced in as typed |
| `modules` | space-separated paths | module manifests to build alongside the case, each passed as `-m`. Paths are relative to `tests_eco/` |
| `stdlib` | `on` (default) / `off` | `off` passes `--no-stdlib`: `die`, `assert` and `mem::` / `math::` become undeclared names |
| `expect` | `ok` (default) / `fail` | the exit status the case must produce |
| `mode` | `run` (default) / `build` | JIT the module, or link a native binary and execute it |

`#` comment lines and blank lines are ignored in the header. All settings must sit above the first
delimiter; a `key: value` line *inside* a section is content.

### Module fixtures

A `modules:` entry names a directory holding a `module.eco` manifest — see
[modules/](modules/), where `lib_geom` depends on `lib_core` and only the former has to be named.

Every case is run with its **own** `--cache-dir` under the build tree. Without that the default store
applies — `.echo` beside each manifest — so the corpus would write artifacts into `tests_eco/` and into
`stdlib/`, and two cases sharing a manifest would share a cache. A case that passes only because an earlier
one populated something is not a case, which is also why it is per case rather than per suite: Catch2 is
free to shuffle them.

**A `.eco` file at or below a directory holding a `module.eco` is not a test case.** It is a source
of that module, claimed by the manifest, so the usual rule that an unpaired `.eco` is an error does
not apply to it. That exception is derived rather than listed: the manifest the runner reads is the
same file `echoc` reads, so a source cannot be claimed by one and orphaned by the other.

### Sections

`OUT` is a byte golden. `IR`, `AST` and `RAST` are directive sections, checked against
`--print-ir`, `--print-ast` and `--print-resolved-ast` respectively.

> **`OUT` is verbatim** — no comment lines, no blank-line stripping, nothing ignored. A `#` in it is a
> `#`. Only a single trailing newline is trimmed, from both sides, so a golden need not be pedantic
> about its final `\n`. Directive sections are the opposite: there, blank lines and `#` / `//` lines
> *are* ignored. That asymmetry is the one thing to keep straight.

An empty `OUT` is fine — a program that prints nothing is a legitimate case. An empty directive section
is an error: it asserts nothing.

### Why the delimiter is matched exactly

`--- NAME --->`, whole line, uppercase name, no leading whitespace. A line that starts with `---` *and*
contains `--->` but does not match exactly — `---OUT--->`, `--- out --->`, `  --- OUT --->` — is a
located error rather than content.

That precision is load-bearing rather than fussy. 74 of the goldens in this corpus **begin with
`---- Issue ----`**, so a `startswith("---")` test would truncate every error test's expectation at
line 1 and leave all 74 passing against one line of diagnostic. Four dashes and no `--->` cannot match
the grammar, and requiring `--->` in the near-miss test is exactly what keeps `---- Issue ----`
content.

## Exit status

`expect` asserts the exit status of the processes the case spawns, and nothing else — no string
sniffing. `ok` means every process exited 0, `fail` means the last one did not.

Two things worth knowing:

- **`fail` does not distinguish a rejected program from a program that called `die`.** Both exit 1:
  under the JIT, `AbortCodegen`'s `exit(1)` takes `echoc` itself down. The `OUT` golden tells them
  apart unambiguously — one contains `---- Issue ----`, the other a panic message — so `expect` does
  not need to.
- In `mode: run`, `Backend::run_code` discards the JIT'd `main`'s return value, so a non-zero `return`
  from a program is invisible. In `mode: build` it is the real process status. Moot today, since the
  only non-zero exit a program can produce is an abort.

## Checking a dump

Some contracts are invisible from program output. That a string literal is a *constant* rather than an
allocation, that `echo` of a string reaches `write` and not `printf`, that a `string::view` costs no
reference count, that a drop landed where the ownership pass said it would — every one of those prints
exactly the same thing whether it holds or not.

`IR`, `AST` and `RAST` assert them with LLVM's FileCheck syntax, reduced to the two directives that
carry their weight:

```
CHECK:     <substring>    must appear at or after the previous CHECK's match
CHECK-NOT: <substring>    must not appear between the surrounding CHECKs
```

Blank lines and `#` / `//` lines are ignored. Anything else is an **error**, not a no-op — a mistyped
`CHEK:` that silently checks nothing is the one failure mode this must not have.

**Directives rather than a byte-for-byte golden, deliberately.** The IR carries a `target triple` and
`target datalayout` that are machine specific, `attributes #0` that depend on the LLVM version, and
`%0`/`%1` numbering that shifts with any unrelated codegen change. A full-text dump golden would fail
on every machine but the one that recorded it and churn on every commit — and a golden regenerated
without being read asserts nothing.

**`CHECK:` advances a cursor**, so order is part of the assertion and each CHECK can only match at or
after the previous one. That is what gives free function scoping — a `CHECK: define i32 @main()`
followed by CHECKs that can then only match inside it — and it is why `CHECK-NOT:` is scoped to the
region *between* its neighbours rather than to the whole dump. Whole-dump would be useless here:
`mem::` declares `@malloc` and the class runtime calls it, both above `main`, so "this function does
not allocate" has to mean "not in this region".

Each section is a separate `echoc` invocation, so `--print-ir` can never pollute the `OUT` golden, and
a failure names which of the case's contracts broke. They cannot share one invocation: `-a` and `-ar`
print byte-identical module headers, so there would be nothing to split a combined stream on, and on a
rejected program `-p` never runs at all.

**`-a` and `-ar` dump every module, the standard library included** — around 650 lines of it in front
of a five-line program, most of that `stdlib/core/string.eco`. So an `AST` or `RAST` section should either set `stdlib: off` or open with

```
CHECK: Module: [main]
```

or a `CHECK: function foo` can quietly match something in `stdlib/math/functions.eco`. This is the
strongest practical reason `stdlib: off` exists;
[`no_stdlib/program_without_stdlib.test`](no_stdlib/program_without_stdlib.test) is the worked example.

## `mode: build`

Links a native binary through `Backend::make_exec` and runs it. Nothing else in this corpus reaches
that path, so [`native/`](native/) is its coverage. Needs bare `clang` on `PATH`; a missing `clang` is
a failure, not a skip.

One thing differs from `mode: run`, and it is not cosmetic — **these are not two views of one
program**: `echoc build` defaults to `--release`, so `assert` and the `ptr<T>` → `T&` null check are
**elided**. Write `flags: --debug` if you want run-mode semantics.

Optimization is *not* a difference. Both subcommands optimize only when passed `-O`, so an `IR`
section in build mode reads release IR that the optimizer has not touched unless the case asks for it
with `flags: -O`. (`build` used to optimize unconditionally and ignore the flag, which meant there was
no way to see what codegen had actually emitted for a release build.)

So write build-mode cases *for* build mode rather than converting existing ones. The `OUT` golden is
the **program's** output only — the build step is asserted through its exit code alone, because its
stdout carries absolute object and executable paths that no golden could hold. Compile-time
diagnostics are `mode: run`'s business.

## Adding a test

1. Write the program, e.g. `tests_eco/math/gcd.eco`.
2. Run it and look at what it printed:
   ```bash
   ./build/echoc run tests_eco/math/gcd.eco
   ```
3. Write `tests_eco/math/gcd.test` with that output under `--- OUT --->`, and **read it** — capturing
   output does not prove it correct, it only records current behavior. Don't commit an expectation that
   encodes a bug.
4. Add `expect: fail` if the case is a deliberate rejection or a panic.
5. `./build/tests "[e2e]"` — the new pair is picked up automatically.

If the case needs flags, run it with those flags in step 2, or the expectation records a different
invocation than the one the runner performs.

Write a directive section by hand from a real dump — `./build/echoc run -p <file>` — and keep it to the
claim you mean. A directive that quotes a whole basic block is a byte-for-byte golden wearing a
disguise.

## Running

```bash
cmake --build build --target tests -j16      # also builds echoc (test target depends on it)
./build/tests "[e2e]"                         # run just the e2e suite
ctest --test-dir build --output-on-failure    # run via ctest
```
