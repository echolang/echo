# Plugin host

Three modules, one contract. `greeter_api` declares the `extern interface`.
`greeter` answers it as `libenglish`. `host` loads the table and calls through
it.

```bash
echoc build                      # in greeter/
echoc build                      # in host/
./ecobuild/host ../greeter/ecobuild/
```

That prints `5` then `42`. The path is a directory; `dynlib::load` adds
`file_name('english')` (`libenglish.dylib` / `libenglish.so` / `english.dll`).

Rebuild `greeter` against an edited `greeter_api` without rebuilding the host
and the host prints an `abi_mismatch` instead of jumping through the wrong
slots.
