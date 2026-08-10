# Echo Programming Language

[![Tests Status](https://github.com/echolang/echo/actions/workflows/cmake-tests.yml/badge.svg)](https://github.com/echolang/echo/actions/workflows/cmake-tests.yml)

Echo is a statically typed, natively compiled, general-purpose programming language.

Welcome to my highly opinionated and far from production-ready version of PHP that goes brrrr.

Echo won't run PHP right now, nor in the future. Echo is fundamentally different but due to its syntax very easy to be picked up by PHP / Java people.

## Installation

```bash
curl -fsSL https://raw.githubusercontent.com/echolang/echo/master/install.sh | bash
```

`echoc run hello.eco` to jit stuff.

Compiling to a native `echoc build` needs `clang` on your `PATH`; on macOS that means the Xcode command line tools (`xcode-select --install`).
