# Conditional compilation

Some code only makes sense on one platform. The path to the running executable is the clearest case: macOS
answers it with `_NSGetExecutablePath`, Linux with `readlink("/proc/self/exe")`, Windows with
`GetModuleFileNameW`. Three names, and **each one exists only on its own platform** — so a program cannot
simply declare all three and pick at runtime. A declaration of a symbol that is not there is a link error,
and it does not matter that the branch naming it never runs.

So the branch has to be gone before the linker sees it. Which means before codegen. Which means the language
has to be able to say it:

```echo
#[if: os == darwin]
extern {
    function _NSGetExecutablePath as c_exe_path(ptr<uint8> $buf, ptr<uint32> $size) : int32;
}

function exe() : string { ... }
#[elif: os == linux]
extern {
    function readlink as c_readlink(ptr<const uint8> $p, ptr<uint8> $buf, usize $n) : int64;
}

function exe() : string { ... }
#[else]
function exe() : string { return str::from_c_str(arg(0)->c_str()); }
#[end]
```

Three declarations of one function, and nothing collides — because the two that don't apply are never read. As
ordinary declarations they'd be a duplicate-signature error.

## The four directives

`#[if:]` opens a region, `#[elif:]` adds an arm, `#[else]` catches what's left, `#[end]` closes it. Only the
first arm whose condition holds is kept; the rest are dropped whatever they say. They nest.

They're written with the same `#[...]` brackets as `#[inline]` and `#[core:]`, but they aren't attributes:
an attribute attaches to the declaration after it, and these bracket a *region*.

## What a condition can test

| Term | Values | An unknown one |
|---|---|---|
| `os` | `darwin`, `linux`, `windows` | is an error |
| `arch` | `arm64`, `x86_64` | is an error |
| a bare name | whatever `--define NAME` declared | is **false** |

```echo
#[if: os == darwin]                       // an axis test
#[if: os != windows]                       // and its negation
#[if: arch == arm64]
#[if: TRACE]                               // a flag, from --define TRACE
#[if: os == darwin || os == linux]         // or
#[if: os == linux && arch == arm64]        // and
#[if: TRACE && !(os == windows)]           // grouping, and unary not
```

`&&` binds tighter than `||`, and `!` tighter than both — the precedence you'd expect. Values are bare names,
not quoted strings: nothing in a condition is Echo source, so a string literal would only suggest it might be.

That rule isn't local to conditions — it holds for everything written inside a `#[...]`. A bare name means
itself there, never a constant and never a type, which is why `#[core: array]` and `#[link: framework
"OpenGL"]` are spelled the way they are. [Modules](modules.md#values) has the rest of the shapes an
attribute value can take.

**Both halves of an axis test are checked, and that's the point of naming the axis.** `#[if: os == darwn]`
is a typo, and it is reported:

```
line 4: unknown os 'darwn', expected one of: darwin, linux, windows
```

Had you written a bare `#[if: darwn]`, nothing could have told that from a flag nobody defined — it would be
false, the block would vanish, and a platform's whole implementation would go with it in silence. That's the
one failure mode this feature could have that nothing else would notice, so the vocabulary is closed.

A *flag* has to work the other way round. `#[if: TRACE]` has to be askable when nobody passed `--define TRACE`,
or it could never take its `#[else]` arm — so an undefined flag is simply false.

## Excluded code is never parsed

Not "parsed and discarded" — never read at all. The filter runs on the token stream between lexing and the
first parse pass, so an arm that does not apply reaches no parser:

```
lex  →  conditional filter  →  pass 1: type names  →  pass 2: declarations  →  pass 3: bodies
```

That is what lets an excluded region name things that **do not exist here**: a symbol only Linux has, a type
only Windows declares. Nothing has to resolve it, because nothing looks at it.

The price is C's price, and it's worth stating plainly: **a mistake inside another platform's region is
invisible on yours.** A typo, a wrong type, a call to a function that doesn't exist — none of it is checked
until something compiles that arm. Which is what the next section is for.

It also means a condition sees only the target and the command line. Nothing has been declared yet, which the
last section of this chapter comes back to.

## Checking the other platform

`--target-os` and `--target-arch` change what conditions see, and nothing else:

```bash
echoc run --target-os linux app.eco       # compile the Linux arm, here
echoc run --target-arch x86_64 app.eco
echoc run --define TRACE app.eco
```

**This is not cross-compilation.** The code is still compiled for the machine you're on, so a foreign arm will
usually fail — at link, if it names a symbol this platform lacks, or at runtime, if it reads a path that isn't
there. Failing is fine. What you're checking is that the arm *parses, resolves and type-checks*, which is the
only verification a platform-specific region ever gets from a machine that isn't that platform.

This is not hypothetical. The first time `exe()`'s Linux arm was compiled this way, it didn't build — a `->` on
a string literal, which has no storage to be a receiver. Nothing on a Mac would have found that.

## Where the conditions live

Anywhere. Because the filter works on tokens, it doesn't care what the region contains:

```echo
// at file level, around declarations
#[if: os == windows]
struct handle { usize $raw; }
#[end]

function measure() : usize
{
    // inside a body
    #[if: TRACE]
    echo 'measuring';
    #[end]

    return 0;
}

struct config
{
    usize $size;

    // inside a struct body
    #[if: os == linux]
    int32 $fd;
    #[end]
}
```

Line numbers survive a filtered region — tokens are dropped, not text, and each token carries its own line — so
a diagnostic after one still names the line you're looking at.

## The module cache

Which declarations a module has depends on the target and on the defines, so both are folded into every module's
cache key alongside the target triple. Two builds differing only in a `--define` produce different objects and
can't be confused for one another.

## Gating a manifest

**A manifest is gated like any other file**, because a `module.eco` is Echo read through the same filtered
path — so a `#[sources:]` line can sit inside an `#[if:]` and needs no manifest grammar of its own:

```echo
#[module: "gated"]

#[if: os == darwin]
#[sources: "dar/*.eco"]
#[elif: os == linux]
#[sources: "lin/*.eco"]
#[end]
```

The facts a manifest's conditions read are the *invocation's*, not the host's — the same ones its sources will
be parsed with. So `--target-os linux` picks the Linux file list *and* compiles those files as Linux, rather
than mixing one platform's sources with the other's filtering.

## A branch inside a body: `const if`

Everything above is about which code is *read*. There's a second question, and `#[if:]` can't answer it:
which of two statements inside a body should be **compiled**, when the answer depends on a type.

```echo
const if (mem::is_trivially_copyable<T>()) {
    mem::copy<T>($this->data, $other->data, $other->len);
}
else {
    // copied one element at a time, so each gets whatever its own copy means
}
```

That's real code from `array<T>`'s copy constructor. Both arms are correct for every `T` — the branch only
buys back the bulk copy for element types that can take one. What `const` adds is that the arm that loses is
**gone**: not compiled, not emitted, not there.

`else` and `else const if` chain the way you'd expect:

```echo
const if (A) { }
else const if (B) { }
else { }
```

And the same marker works on a value, where it means "work this out now":

```echo
int32 $slots = const(4 * 8);          // the literal 32 reaches the compiler's output
```

### The compiler says so when it can't

That's the point of writing `const` rather than hoping. A condition the compiler cannot decide is an error
where you wrote it:

```
const if ($n > 0) {
```
```
line 5: this is not something the compiler can work out for itself - it needs storage, a call, or
something that only exists while the program runs.
```

An ordinary `if` would have compiled that into a runtime branch, which is exactly the silence this avoids.
The same holds for a value: `const(255 + 1)` at a `uint8` is refused rather than wrapped to 0 — a `const`
value is one the compiler stands behind.

### What a condition can test

Anything the compiler already knows: `mem::is_trivially_copyable<T>()` and `mem::needs_destruction<T>()`,
`true` and `false`, whole-number literals, a [constant](constants.md), the comparisons, `&&` and `||`, and
`+ - * / %`.

What it can't: anything with storage (a `$variable`, a field), an ordinary function's result, a floating
point value, and — for now — `mem::size_of<T>()` and `mem::align_of<T>()`, which the compiler only works out
once it is emitting code for a particular machine.

### Which of the two to reach for

| | |
|---|---|
| `#[if:]` | code that must not be **read** — a symbol this platform doesn't have, a whole `extern` block, a `struct` that only exists on Windows |
| `const if` | a statement inside a body that must not be **compiled** — and the thing `#[if:]` can never do: name a type parameter |

### One thing to know about the arms

**An arm is a block**, so it can't declare something the code after it uses. Unlike C's `#if`, which pastes
tokens, this selects a *scope*:

```echo
usize $size = 0;                    // declare out here

const if (mem::needs_destruction<T>()) {
    $size = 1;                      // and assign inside
}
```

That's the one place `const if` is genuinely weaker than a token-level directive.

### And one thing it can do that a runtime `if` cannot

A [move](ownership_and_moving.md) inside an arm is an *unconditional* move, because the arm is the only code
there is. Inside a runtime `if` the same `mv` is a conditional move, and the compiler says so.

## Two things a condition deliberately can't do

**It can't name a constant, or any other declaration.** Not an omission — a consequence of running before
anything is declared:

```echo
#[if: MAX > 10]     // not possible
```

The filter decides which regions are parsed *at all*, and a constant's name is published by a later pass. That
ordering is what lets an excluded region mention symbols this platform doesn't have, so it isn't a trade I'd
give up for this. See [Constants](constants.md), which composes the other way round quite happily: a constant
inside an `#[if:]` region is dropped with the rest of it.

**`const if` is the answer to this one**, and it can name a constant precisely because it runs after
everything is declared — see the section above.

**A define carries no value.** `--define NAME=VALUE` is refused rather than quietly ignored — a flag answers
"is this on", and naming a *value* is what a [`const` declaration](constants.md) is for.
