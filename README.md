# Echo Programming Language

[![Tests Status](https://github.com/echolang/echo/actions/workflows/cmake-tests.yml/badge.svg)](https://github.com/echolang/echo/actions/workflows/cmake-tests.yml)

Echo is a statically typed, natively compiled language. The syntax looks like PHP. LLVM gives you a real binary, and there is no runtime to ship beside it.

Welcome to my highly opinionated and far from production-ready version of PHP that goes brrrr.

Under the syntax, this is what is actually there:

- **Structs** - values. Assigning one copies it. You call the type, there is no `new`.
- **Classes** - the heap half. Reference counted, shared when you assign, destroyed when the last owner goes away.
- **Generics** - monomorphized. `largest<T: numeric>` compiles to a real function per type, with no dispatch in the middle.
- **Interfaces** - two jobs. A constraint a generic can name, and a value only a class can be stored as.
- **Enums** - a tag plus an optional payload. `match` has to cover the cases.
- **Ownership** - one owner. Giving a value away is spelled `mv`, and the call site has to say it too.
- **Nullability** - non-null unless you write `T?`. `null` does not fit in a plain `int32`.
- **Operators** - `+` and `[]` are declarations you can write yourself. Suffix ones too, so `1m + 50cm` can mean something.
- **Closures** - `function<int32(int32)>` is a value. A closure captures what it reads, by value.
- **Arrays and maps** - `array<T>` holds one type. `map<K, V>` wants a hash and an `==`. Neither of them is a PHP array.
- **Pointers** - `ptr<T>` when you want the address. The dangerous arithmetic has to sit in `unsafe`.

## It will not run your PHP

Echo does not run PHP, and it never will. The syntax is borrowed. The types, ownership, and memory model are a different language.

It is a hobby. The standard library is small, the type system has holes, and the compiler has bugs. I would rather you bounce off this page than off an error six hours in.

## A program

A file is a program. No `main`, no class, no imports.

```echo
echo "Hello, Echo!";
```

```bash
echoc run hello.eco
```

```
Hello, Echo!
```

Dialing a gate!

```echo
enum Lock
{
    case milkyWay(int32 $chevrons);
    case pegasus(int32 $chevrons);
}

enum DialError
{
    case incomplete;
    case irisClosed;
}

function dial(const string& $address) : result<Lock, DialError>
{
    if ($address->empty()) {
        return .error(.incomplete);
    }

    if ($address == 'earth') {
        return .error(.irisClosed);
    }

    if ($address == 'atlantis') {
        return .ok(.pegasus(8));
    }

    return .ok(.milkyWay(7));
}

function report(const string& $address) : void
{
    Lock $gate = guard dial($address) else ($why) {
        echo match ($why) {
            .incomplete => 'not enough glyphs',
            .irisClosed => 'iris is closed',
        };
        return;
    };

    echo match ($gate) {
        .milkyWay($n) => "{$address}, {$n} chevrons, Milky Way",
        .pegasus($n) => "{$address}, {$n} chevrons, Pegasus",
    };
}

report('atlantis');
report('abydos');
report('earth');
```

```
atlantis, 8 chevrons, Pegasus
abydos, 7 chevrons, Milky Way
iris is closed
```

## Install

A release is two binaries, `echoc` and `epm`. The standard library is inside `echoc`.

macOS and Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/echolang/echo/master/install.sh | bash
```

That lands in `/usr/local/bin`, and uses `sudo` only if that directory is not yours. Somewhere else:

```bash
curl -fsSL https://raw.githubusercontent.com/echolang/echo/master/install.sh | ECHO_INSTALL_DIR="$HOME/.local/bin" bash
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/echolang/echo/master/install.ps1 | iex
```

That puts `echoc` and `epm` under `%LOCALAPPDATA%\echo\bin` and on your user `PATH`. Open a new terminal. The one you already have will not see it.

Or download `echo-windows-x86_64-setup.exe` from the [latest release](https://github.com/echolang/echo/releases/latest). The zip is there if you want to unpack it yourself. A Windows release already includes clang, lld-link, and a sysroot.

```bash
echoc --version
epm --version
```

### Prebuilt platforms

| Platform | Asset |
|---|---|
| macOS on Apple Silicon | `echo-macos-arm64` |
| Linux on x86_64 | `echo-linux-x86_64` |
| Windows on x86_64 | `echo-windows-x86_64` |

No Intel Mac, no Linux on ARM, no Windows on ARM. The script says so and stops. On those machines, [build from source](#building-this-repo).

`epm` needs `git` on your `PATH`.

## Two commands

```bash
echoc run hello.eco
echoc build -o hello hello.eco
./hello
```

| | `echoc run` | `echoc build` |
|---|---|---|
| Default | `--debug` | `--release` |
| `assert` and runtime checks | kept | dropped |
| Optimizer | on, per module | on, per module |
| Output | runs in this process | a native binary |
| Needs `clang` | no | yes |

An `assert` that fires under `run` can pass silently under `build`. You can flip either command:

```bash
echoc run --release hello.eco
echoc build --debug -o hello hello.eco
```

The binary runs on another machine of the same platform, with no Echo installed there.

`build` needs `clang`. On macOS that is `xcode-select --install`. On Linux, install it from the distro. A Windows release already has it. Without clang, `run` still works and `build` does not.

`echoc test` compiles `test` blocks. `run` and `build` leave them out. 

## The manual

The language lives at [echoc.dev](https://echoc.dev). Start with [what Echo is](https://echoc.dev/guide/what-is-echo), [your first program](https://echoc.dev/guide/first-program), [a tour](https://echoc.dev/guide/tour), or [coming from PHP](https://echoc.dev/guide/coming-from-php).

Point an editor at `echoc lsp`, or install the [VS Code extension](https://github.com/echolang/echolang-vscode). 

## License

Apache License 2.0. See [LICENSE](LICENSE).
