# `tests_eco/` — end-to-end golden-output tests

Each test is a pair of sibling files that may live in **any nested subdirectory**:

```
tests_eco/<anything>/<name>.eco       # the Echo program to run
tests_eco/<anything>/<name>.eco.out   # its expected output (the "golden")
tests_eco/<anything>/<name>.eco.flags # optional: extra echoc flags for this case
```

The runner ([`tests/e2e_eco.cpp`](../tests/e2e_eco.cpp)) walks this tree recursively, runs
each `.eco` through the real compiler (`echoc run <file>`), captures its combined
stdout+stderr, and compares it against the matching `.eco.out`. Discovery is done at
**runtime**, so adding, moving, or nesting test pairs never requires reconfiguring CMake.

## Matching rules

- Output is compared **byte-for-byte**, after trimming a single trailing newline from both
  sides (so goldens don't need to be pedantic about the final `\n`).
- Matching is **uniform for success and failure**. The compiler prints its diagnostics to
  stdout, so a deliberately broken program is tested exactly like a working one — its
  `.eco.out` just contains the expected diagnostic block (see
  [`errors/undeclared_variable.eco.out`](errors/undeclared_variable.eco.out)).
- The exit code is **not** asserted — the captured output is the entire contract.
- A sibling **`.eco.flags`** file, if present, supplies extra flags spliced in after `run`.
  Most cases need none — the point of a golden is that one fixed invocation gives one fixed
  output. Use it when the flag *is* the thing under test, which no amount of source can show
  from inside the program: `panic/assert_release_elided.eco.flags` holds `--release`, and the
  golden proves the program ran straight past a false `assert`.

## Adding a test

1. Write the program, e.g. `tests_eco/math/gcd.eco`.
2. Generate its golden by capturing the real output:
   ```bash
   ./build/echoc run tests_eco/math/gcd.eco > tests_eco/math/gcd.eco.out 2>&1
   ```
3. **Read the golden and confirm it is correct** — capturing does not prove correctness, it
   only records current behavior. Don't commit a golden that encodes a bug.
4. Run `./build/tests "[e2e]"` (or `ctest`) — the new pair is picked up automatically.

If the case needs flags, write them to `<name>.eco.flags` first and capture with the same
flags, or the golden records a different invocation than the one the runner performs.

## Checking the generated IR

Some contracts are invisible from program output. That a string literal is a *constant* rather than an
allocation, that `echo` of a string reaches `write` and not `printf`, that passing a `string::view`
touches no reference count — every one of those prints exactly the same thing whether it holds or not.

An optional sibling **`<name>.eco.ir`** asserts them against the emitted LLVM IR. When it exists the
runner performs a *second* `echoc run --print-ir` (separate from the output run, so `--print-ir` can
never pollute the `.eco.out` golden) and applies the directives in it:

```
CHECK:     <substring>    must appear at or after the previous CHECK's match
CHECK-NOT: <substring>    must not appear between the surrounding CHECKs
```

Blank lines and `#` / `//` lines are ignored. Anything else is an **error**, not a no-op — a mistyped
`CHEK:` that silently checks nothing is the one failure mode this must not have. An empty file is an
error for the same reason.

**Directives rather than a byte-for-byte golden, deliberately.** The IR carries a `target triple` and
`target datalayout` that are machine specific, `attributes #0` that depend on the LLVM version, and
`%0`/`%1` numbering that shifts with any unrelated codegen change. A full-text IR golden would fail on
every machine but the one that recorded it and churn on every commit — and a golden regenerated without
being read asserts nothing.

**`CHECK:` advances a cursor**, so order is part of the assertion and each CHECK can only match at or
after the previous one. That is what gives free function scoping — a `CHECK: define i32 @main()`
followed by CHECKs that can then only match inside it — and it is why `CHECK-NOT:` is scoped to the
region *between* its neighbours rather than to the whole module. Whole-module would be useless here:
`mem::` declares `@malloc` and the class runtime calls it, both above `main`, so "this function does not
allocate" has to mean "not in this region".

```
CHECK: define i32 @main()
CHECK: store %string { %view { ptr @str, i64 3 }, ptr null }
CHECK-NOT: call ptr @malloc
CHECK-NOT: strong.inc
CHECK: ret i32 0
```

Write these by hand from a real dump — `./build/echoc run -p <file>` — and keep them to the claim you
mean. A directive that quotes a whole basic block is a byte-for-byte golden wearing a disguise.

## Running

```bash
cmake --build build --target tests -j16      # also builds echoc (test target depends on it)
./build/tests "[e2e]"                         # run just the e2e suite
ctest --test-dir build --output-on-failure    # run via ctest
```
