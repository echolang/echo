# Echo Programming Language

[![Tests Status](https://github.com/echolang/echo/actions/workflows/cmake-tests.yml/badge.svg)](https://github.com/echolang/echo/actions/workflows/cmake-tests.yml)

Echo is a statically typed, natively compiled, general-purpose programming language.

Welcome to my highly opinionated and far from production-ready version of PHP that goes brrrr.

Echo won't run PHP right now, nor in the future. Echo is fundamentally different but due to its syntax very easy to be picked up by PHP / Java people.

## Installation

macOS / Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/echolang/echo/master/install.sh | bash
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/echolang/echo/master/install.ps1 | iex
```

Or download `echo-windows-x86_64-setup.exe` from the [latest release](https://github.com/echolang/echo/releases/latest) and run the wizard. The zip is there if you want to unpack it yourself.

`echoc run hello.eco` to jit. `echoc build hello.eco` emits a native binary. On macOS that needs the Xcode command line tools (`xcode-select --install`). On Windows the installer already ships clang, lld-link and a sysroot; no separate LLVM install.
