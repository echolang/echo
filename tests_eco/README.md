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
CHECK: define i32 @main(
CHECK-NOT: call ptr @__eco_alloc
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
| `stdlib` | `on` (default) / `off` | `off` passes `--no-stdlib`: `die`, `assert` and `mem::` / `std::` become undeclared names |
| `expect` | `ok` (default) / `fail` / an exit status like `3` | the exit status the case must produce |
| `mode` | `run` (default) / `build` | JIT the module, or link a native binary and execute it |
| `env` | space-separated `KEY=VALUE` pairs | set in the environment of everything the case spawns |
| `args` | space-separated words | the program's own arguments — `argv[1]` onwards |

`expect` takes an **exact status** as well as `ok`/`fail`, and that exists because `std::env::exit($code)`
made the status something a program *chooses*: pinned as `fail`, a case asserting `exit(3)` would pass just
as well if the compiler crashed. In `mode: build` an exact status is the *program's* — a build that fails
before producing a binary is reported as a build failure, not matched against it.

`env` and `args` exist for the same reason: without them `std::env` can only be tested against whatever the
machine running the suite inherited, which differs between a developer and CI and asserts almost nothing.
Both are shell-level — a `KEY=VALUE` prefix and an argument suffix on the spawned command — so they reach a
JIT'd program and a linked binary by the same route, and neither can leak into the suite's own environment
or into a parallel case. Neither a value nor an argument may contain a space; a pair without an `=` is a
located error rather than a word the shell would try to run.

### Testing another platform

`flags: --target-os linux` compiles the Linux arm of a `#[if: os == ...]` region on whatever machine runs the
suite — see [book/concept/conditional.md](../book/concept/conditional.md). It is **not** cross-compilation:
the code is still built for the host, so a foreign arm usually fails at link or at runtime, and that is fine.
What the case asserts is that the arm parses, resolves and type-checks, which is the only check a
platform-specific region gets from a machine that is not that platform.

`env/exe_linux_arm` and `env/exe_darwin_arm` are the model, one per arm so that both are checked wherever
the suite runs. Neither *calls* the gated function: a stdlib function is lowered whether or not a program
reaches it, so `--target-os` alone selects the arm, while leaving it unreached keeps the case runnable on a
host it is not for — the JIT prunes what the entry point cannot reach, so a foreign `extern` is never a
symbol anything has to resolve. The assertion is therefore an `IR` section rather than `OUT`, the `--print ir`
module being the unpruned one. Without cases like these, half of every platform-gated file is verified only
by CI on a host the author does not have.

`flags: --define NAME` declares a condition flag the same way.

### Program arguments

`args` reaches a JIT'd program through echoc's `--` separator, which is what tells the driver where its own
command line stops. On the `build` path there is no separator: the binary *is* the program, so its argv is
the program's already. `arg(0)` differs between the two — the source file under `run`, the linked binary
under `build` — so a case asserting it should assert a shape rather than a value.

**Every case is compiled with `--track-allocations`**, which is not a setting because no case wants it
off. Two things follow. `mem::live_allocations()` is readable from any case, so asserting the absence of
a leak needs no header line — see *Checking for a leak* below. And every allocation in the language is
spelled `@__eco_alloc` rather than `@malloc`, uniformly, so an `IR` section naming it cannot be
accidentally vacuous. (An untracked build calls `@malloc` directly; `tests/module_cache.cpp` is what
covers that lowering, since it builds its own commands.)

`#` comment lines and blank lines are ignored in the header. All settings must sit above the first
delimiter; a `key: value` line *inside* a section is content.

### Module fixtures

A `modules:` entry names a directory holding a `module.eco` manifest — see
[modules/](modules/), where `lib_geom` depends on `lib_core` and only the former has to be named.

Every case gets its **own scratch directory** under the build tree, holding its `-o` binary and its
`--build-dir`. It is created empty and wiped afterwards, pass or fail. Without the override the default
applies — `ecobuild` beside each manifest — so the corpus would write artifacts into `tests_eco/` and into
`stdlib/`, and two cases sharing a manifest would share a store. A case
that passes only because an earlier one populated something is not a case, which is also why it is per case
rather than per suite: Catch2 is free to shuffle them.

**A `.eco` file at or below a directory holding a `module.eco` is not a test case.** It is a source
of that module, claimed by the manifest, so the usual rule that an unpaired `.eco` is an error does
not apply to it. The runner tests for the manifest's *presence*, not its contents — so a source under a
module directory that the manifest's `#[sources:]` does not actually name is skipped here too.

### Sections

`OUT` is a byte golden. `IR`, `UNIT_IR`, `AST` and `RAST` are directive sections, checked against
`--print ir`, `--print ir-units`, `--print ast` and `--print ast-resolved` respectively.

> **`IR` is the whole program, `UNIT_IR` is one unit at a time.** `--print ir` folds every compilation
> unit into one module first, because that is the only thing an O3 pipeline or a single dump can look at -
> so an `IR` section always describes the `--optimize whole` build even when the case sets no flags. `UNIT_IR` prints
> each unit as the object writer receives it, which is what an ordinary `echoc build` emits, and each unit
> is preceded by a `[unit <name>]` header to anchor on. It is only meaningful on a **`mode: build`** case:
> `run` merges unconditionally, because the JIT can take only one module.

> **`OUT` is verbatim** — no comment lines, no blank-line stripping, nothing ignored. A `#` in it is a
> `#`. Only a single trailing newline is trimmed, from both sides, so a golden need not be pedantic
> about its final `\n`. Directive sections are the opposite: there, blank lines and `#` / `//` lines
> *are* ignored. That asymmetry is the one thing to keep straight.

An empty `OUT` is fine — a program that prints nothing is a legitimate case. An empty directive section
is an error: it asserts nothing.

`OUT` holds the compiler's **stdout and stderr merged** — the harness runs echoc through `popen` with
`2>&1`. That is why a diagnostic golden is plain ASCII with no configuration: `--diagnostics=auto`
resolves against a pipe. The progress checklist is off for the same reason and needs no `--silent` in
`flags` — a golden holding a spinner frame would be a golden holding the terminal it was recorded on.

### Why the delimiter is matched exactly

`--- NAME --->`, whole line, uppercase name, no leading whitespace. A line that starts with `---` *and*
contains `--->` but does not match exactly — `---OUT--->`, `--- out --->`, `  --- OUT --->` — is a
located error rather than content.

That precision is load-bearing rather than fussy. Error goldens used to **begin with
`---- Issue ----`**, so a `startswith("---")` test would truncate every error test's expectation at
line 1 and leave all of them passing against one line of diagnostic. Four dashes and no `--->` cannot
match the grammar, and requiring `--->` in the near-miss test is exactly what kept `---- Issue ----`
content.

The banner is `[error]` now and the renderer is held to never emitting a line beginning `---`
(`tests/diagnostics.cpp`), so the collision is closed from both ends. The exact-match rule stays anyway:
a diagnostic is not the only thing a case can print.

## Regenerating the goldens

202 of the 544 cases assert a diagnostic byte for byte, so a deliberate change to how one reads is a
change to all of them at once. [tools/regen_eco_goldens.py](../tools/regen_eco_goldens.py) re-runs each
case exactly as `EchoTests::echoc_command` builds its command line and rewrites only the `OUT` body:

```bash
tools/regen_eco_goldens.py            # list what would change
tools/regen_eco_goldens.py --write    # write it
```

**Run it over the whole corpus, never just `errors/`.** The ~340 cases whose `OUT` is program output must
come back byte-identical, and one of them moving is a bug in the change rather than a golden to accept —
which is the only reason to regenerate broadly.

## Exit status

`expect` asserts the exit status of the processes the case spawns, and nothing else — no string
sniffing. `ok` means every process exited 0, `fail` means the last one did not, and a number means it was
exactly that.

Two things worth knowing:

- **`fail` does not distinguish a rejected program from a program that called `die`.** Both exit 1:
  under the JIT, `AbortCodegen`'s `exit(1)` takes `echoc` itself down. The `OUT` golden tells them
  apart unambiguously — one contains `---- Issue ----`, the other a panic message — so `expect` does
  not need to. Write an exact status when the difference matters.
- **A program's own status reaches the suite in both modes**, by two different routes. `mode: build` is an
  ordinary process exit. In `mode: run`, `Backend::run_code` returns what `runFunctionAsMain` returned and
  `main_run` propagates it — but a `die` or a `std::env::exit` never gets that far, because both call libc's
  `exit` from *inside* the JIT'd code and take `echoc` down with them. That is already the right status, so
  the two paths agree; see `env/exit_code` and `env/exit_code_native`, which assert the same `3` through
  each.

## Checking a dump

Some contracts are invisible from program output. That a string literal is a *constant* rather than an
allocation, that `echo` of a string reaches `write` and not `printf`, that a `string::view` costs no
reference count, that a drop landed where the ownership pass said it would — every one of those prints
exactly the same thing whether it holds or not.

`IR`, `UNIT_IR`, `AST` and `RAST` assert them with LLVM's FileCheck syntax, reduced to the two directives that
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
after the previous one. That is what gives free function scoping — a `CHECK: define i32 @main(`
followed by CHECKs that can then only match inside it — and it is why `CHECK-NOT:` is scoped to the
region *between* its neighbours rather than to the whole dump. Whole-dump would be useless here:
the allocation seam is defined above `main` and the class runtime calls it, so "this function does
not allocate" has to mean "not in this region".

Each section is a separate `echoc` invocation, so `--print ir` can never pollute the `OUT` golden, and
a failure names which of the case's contracts broke. They cannot share one invocation: `--print ast` and `--print ast-resolved`
print byte-identical module headers, so there would be nothing to split a combined stream on, and on a
rejected program `--print ir` never runs at all.

**`--print ast` and `--print ast-resolved` dump every module, the standard library included** — around 650 lines of it in front
of a five-line program, most of that `stdlib/core/string.eco`. So an `AST` or `RAST` section should either set `stdlib: off` or open with

```
CHECK: Module: [main]
```

or a `CHECK: function foo` can quietly match something in `stdlib/std/math/functions.eco`. This is the
strongest practical reason `stdlib: off` exists;
[`no_stdlib/program_without_stdlib.test`](no_stdlib/program_without_stdlib.test) is the worked example.

## Checking for a leak

There are two ways, at two genuinely different moments, and there is deliberately no `.test` setting for
either — a leak check is a line of Echo.

**In the program**, with `mem::live_allocations()`, which reads how many allocations the whole program
still holds. It counts *allocations*, not bytes: a class, a closure's captured environment, an `array<T>`'s
buffer and a `mem::alloc` are one each.

```php
{
    Node $n = Node(1);
    assert(mem::live_allocations() > 0);
}
assert(mem::live_allocations() == 0);
```

**Read it after a scope, not at the end of a file.** A declaration at module scope is released *after* the
last statement, so a check written there still counts its own owners — which is why the block above is a
block. This is the one thing authors get wrong, and it fails as an assertion firing rather than as a
silently passing one. Exit status and the `OUT` golden already assert the result, and it works in `run`
and `build` alike — except that `build` is a release build, where `assert` is elided and asserts nothing.

**Or with `flags: --explain memory`**, which has `main` print a `[memory]` section on its way out. This is the only way
to see the state *past* the program's own module-scope teardown, so it is the whole-program answer:

```
flags: --explain memory

--- OUT --->
7
[memory]
  0 live allocations
```

A `die` at module scope prints no `[memory]` line — the program already stopped, so there is no teardown
to report on ([`memory/no_report_after_a_die.test`](memory/no_report_after_a_die.test) pins that).
[`memory/explain_memory.test`](memory/explain_memory.test) is the worked example, and
[`memory/leak_is_visible.eco`](memory/leak_is_visible.eco) is the contrast case — a counter that only ever
reads zero would prove nothing, so one case leaks on purpose and reads the leak.

## `mode: build`

Links a native binary through `Backend::make_exec` and runs it. Nothing else in this corpus reaches
that path, so [`native/`](native/) is its coverage. Needs bare `clang` on `PATH`; a missing `clang` is
a failure, not a skip.

One thing differs from `mode: run`, and it is not cosmetic — **these are not two views of one
program**: `echoc build` defaults to `--release`, so `assert` and the `ptr<T>` → `T&` null check are
**elided**. Write `flags: --debug` if you want run-mode semantics.

Optimization is *not* a difference. Both subcommands optimize only when passed `--optimize whole`, so an `IR`
section in build mode reads release IR that the optimizer has not touched unless the case asks for it
with `flags: --optimize whole`. (`build` used to optimize unconditionally and ignore the flag, which meant there was
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

Write a directive section by hand from a real dump — `./build/echoc run --print ir <file>` — and keep it to the
claim you mean. A directive that quotes a whole basic block is a byte-for-byte golden wearing a
disguise.

## Running

```bash
cmake --build build --target tests -j16      # also builds echoc (test target depends on it)
./build/tests "[e2e]"                         # run just the e2e suite
ctest --test-dir build --output-on-failure    # run via ctest
```
