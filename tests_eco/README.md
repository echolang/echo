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

## Running

```bash
cmake --build build --target tests -j16      # also builds echoc (test target depends on it)
./build/tests "[e2e]"                         # run just the e2e suite
ctest --test-dir build --output-on-failure    # run via ctest
```
