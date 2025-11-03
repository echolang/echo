# Modules

A program grows out of one file. Two, then a directory, then a directory you'd like to use from a
*different* program — and at that point the command line stops being a good way to say what your program is
made of:

```bash
echoc build -o app src/*.eco ../geom/src/*.eco ../geom/src/detail/*.eco vendor/json/*.eco
```

That line is a build system written in a shell, and it has to be right in every place it's typed. Here's
the same program:

```bash
echoc build -o app
```

The rest of this chapter is about the file that makes the difference, what a module *is*, and why the
compiler can skip work the second time you build.

## A project is a directory with a manifest

Create `module.eco` beside your sources:

```echo
#[module: "myapp"]
#[version: "0.1.0"]

#[sources: "src/*.eco"]
```

That's a complete manifest. `echoc run` and `echoc build -o app` in that directory find it, compile every
file it names, and run or link the result. No arguments, no flags.

The manifest is **written in Echo** — that's why it's called `module.eco` and not `module.toml`. Your editor
already highlights it, the compiler already knows how to read it, and there's no second grammar to learn or
maintain. It's read by the real lexer, into a scratch bundle that is thrown away; nothing it declares reaches
your program.

Four attributes, and between them the whole format:

| | |
|---|---|
| `#[module: "name"]` | what this module is called. Required, and unique within a build |
| `#[version: "0.1.0"]` | recorded, and part of the build fingerprint. Nothing resolves against it yet |
| `#[sources: "pattern"]` | files this module is made of. Repeatable |
| `#[depends: "path"]` | another module this one needs. Repeatable |

Anything the format doesn't understand is an error, never a silently ignored line. Why be that strict? Because
the alternative fails where you can't see it. A misspelled `#[source:]` gives you a module with no files and no
complaint. A `#[sources:]` pattern matching nothing gives you an empty module that compiles and does nothing at
all. Both are refused, with the line number.

A pattern is relative to the manifest, never to wherever you happened to run `echoc` from — so a library means
the same thing when you build it directly and when somebody else depends on it. `*` matches within one
directory, `**` descends:

```echo
#[sources: "src/*.eco"]
#[sources: "src/**/*.eco"]
```

One thing worth knowing: a manifest is never one of its own sources. `#[sources: "*.eco"]` in a directory
holding `module.eco` matches it, and compiling it would declare its own attributes into your program — so it's
excluded for you. The obvious pattern is the one that would otherwise break.

## Using a library

A library is a directory with a manifest. Point at it:

```bash
echoc build -o app -m ../geom app.eco
```

and its namespaces are simply available:

```echo
geom::Point $p = geom::Point(3.0, 4.0);
echo $p->length();
```

There's no import statement, and nothing to write at the top of your file. A module's declarations are
visible to every module compiled after it, which is what makes `-m` the whole of the surface.

When a library needs another library, it says so itself rather than making you say it:

```echo
#[module: "geom"]

#[depends: "../core"]

#[sources: "src/*.eco"]
```

Now `-m ../geom` pulls in `../core` too, once, in the order that works. A `#[depends:]` entry may name the
manifest file or the directory holding it — both read naturally, and only one of them survives moving the
file.

Two things to know before you ship one of these to somebody else.

**A library is source, not a package.** A cached object is a local build artifact — there's no registry, no
version constraint and no lockfile, so sharing a library means sharing its source. `#[version:]` is recorded and
fingerprinted, and resolves nothing; it's there so a change to it invalidates a build, not so anything can ask
for a range.

**Operators are always global.** An operator declaration lands in the one program-wide set whichever module
declares it, because a symbol has to bind the same way in every file or two files would read the same expression
differently. So a library shipping `operator +` puts it in everyone's — worth being deliberate about, and worth
knowing when you go looking for where an unexpected `+` came from.

## Chunking a large program

The same mechanism splits an application that has grown too slow to rebuild. Give each part a manifest and a
dependency edge:

```
myapp/
  core/module.eco     #[module: "core"]
  data/module.eco     #[module: "data"]   #[depends: "../core"]
  ui/module.eco       #[module: "ui"]     #[depends: "../data"]
  module.eco          #[module: "myapp"]  #[depends: "./ui"]
```

Each chunk is compiled and cached on its own, so editing `ui` doesn't recompile `core`. One manifest is one
module; chunking is just more manifests.

## The order is a rule, not a detail

Here's the rule: a module can name symbols from any module compiled before it, and from none compiled after it.

That's not a restriction invented for modules. It falls out of how the compiler reads code — all three parse
passes run over a whole module before the next one starts. So by the time your file is read, an earlier module's
declarations exist and a later module's don't.

Two consequences:

- **The dependency graph has to be a DAG.** A cycle is refused with the path that forms it, rather than broken
  somewhere arbitrary. Two modules that need each other are one module.
- **The standard library comes first**, always, which is why you never write `#[depends: "stdlib"]`. It's an
  ordinary manifest module — `stdlib/module.eco`, no privileges — that the compiler adds as the first root of
  every build unless `--no-stdlib` says otherwise.

Order is decided by what you pointed at, in the order you pointed at it, with each module's dependencies
placed ahead of it. I deliberately didn't derive it from where files sit on disk: that made a library compile
or fail depending on where its checkout happened to live, which is a lovely way to lose an afternoon.

## Building the second time

Compiling a library you didn't change is wasted work, so `echoc build` doesn't do it twice. Each module's
compiled object is stored, and reused when nothing that could affect it has changed:

```bash
$ echoc build -o app -m ../geom -ec app.eco
[cache]
  stdlib  c5e7618b5d8fc44f  hit
  core    a30f7b1c9de44210  hit
  geom    e0d848a3cdbb155f  miss  ('point.eco' changed)
```

`-ec` is how you ask what happened, and a miss names the file rather than reporting a number you can't act on.

Artifacts live in `.echo` beside each manifest. So a library's cache travels with the library, deleting a
checkout deletes its cache, and no two projects can be confused for one another. `--cache-dir` puts them
somewhere else.

Two exceptions to "beside the manifest", both because a cache must never get in the way. The standard library
ships with the compiler rather than with your program, so it caches under `$XDG_CACHE_HOME/echo` (or
`~/.cache/echo`) instead of in the toolchain's own directory. And a store that can't be written — a read-only
library, an installed compiler, a full disk — isn't an error: the module is compiled and simply not kept,
reported as `(its cache directory is not writable)`.

What invalidates a module:

- its own sources or manifest
- any dependency
- any module compiled before it
- the build mode, the target, or the compiler version

That last line matters more than it looks. An object built with assertions is not an object built without them,
so `--debug` and `--release` artifacts coexist rather than overwrite each other.

The rule about preceding modules is stricter than a dependency list, and deliberately so. Because a module can
name anything declared before it, a module you didn't declare a dependency on can still change what your code
resolves to — one more overload in a set you call into is enough. Rebuilding is cheap and being wrong is not.

## What the cache doesn't do

**An optimized build doesn't use it.** `-O` folds every module into one unit before optimizing, because the
inliner can only work on a body it can see; per-module objects and whole-program optimization are exclusive,
and `-ec` says `bypassed` rather than pretending otherwise.

If you want a function inlinable across a module boundary regardless, mark it:

```echo
#[inline]
function length_sq(float32 $x, float32 $y) : float32
{
    return $x * $x + $y * $y;
}
```

`#[inline]` means **copy this body into every unit that calls it** — the same treatment a generic gets, and the
reason a generic needs no marking. It's a placement instruction rather than a promise the optimizer has to
keep, so the inliner is still free to decline.

**`echoc run` doesn't use it either.** The JIT is handed one module, so there are no per-module objects in
play. Use `build` when you want the cache.
