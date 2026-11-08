# Standard library tests

Behavioral tests for the standard library, run with Echo's own `test` blocks.

They live in this checkout and are not embedded in a released `echoc`. The
embedder skips `tests/` the same way it skips `build/` and `sketches/`. A
project's `echoc test` never compiles them: only an invocation whose user root
is this module does.

```bash
./build-test/echoc test -m stdlib --track-allocations
```

`--track-allocations` is required: some tests read `mem::live_allocations()`.
`echoc test` already defaults to debug, so `assert` is how a test fails.

CI runs this job after the build and before the e2e shards, so a stdlib
failure skips the corpus.

`--filter group:core`, `--filter group:std`, `--filter file:array`, or a test
name all work. Compiler goldens in `tests_eco/` stay; they pin diagnostics and
IR, not this suite.
