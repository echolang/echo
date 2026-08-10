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

Seven attributes, and between them the whole format:

| | |
|---|---|
| `#[module: "name"]` | what this module is called. Required, and unique within a build |
| `#[version: "0.1.0"]` | recorded, and part of the build fingerprint. Nothing resolves against it yet |
| `#[sources: "pattern"]` | files this module is made of. Repeatable |
| `#[depends: "path"]` | another module this one needs. Repeatable |
| `#[link: <kind> "value"]` | a native library this module needs linked. Repeatable |
| `#[cc: <kind> "value"]` | C sources that ship with this module. Repeatable |
| `#[build_dir: "path"]` | where this module's build artifacts go. Defaults to `ecobuild` |

`link` and `cc` are what a binding to a real native library is made of, and they travel with the module that
declares them — anything depending on it inherits them without repeating a word. Both *tag* their value with
what kind of thing it is rather than being an attribute per kind, so what a build can be told grows without
this table growing with it. [C binding](cbinding.md) has the kinds and what each one does. `build_dir` is the
opposite kind of thing — it says nothing about your program, only about where the compiler puts what it
makes; see [building the second time](#building-the-second-time).

### Values

An attribute's value is data, and it comes in six shapes: a string, a number, `true` or `false`, a bare
**name**, a `[` list `]`, and a `{` record `}`. A value may be tagged with a name in front of it, which is
how `#[link: framework "OpenGL"]` says what kind of thing it names.

A bare name means *itself* — it is never a constant, a variable or a type. That is the same rule
[conditional compilation](conditional.md) has always followed, where `darwin` in `#[if: os == darwin]` is a
name and not something the program declares. Which is also the rule for when to quote: a closed set the
compiler knows is a bare name, and free text is a string.

**Anywhere one value is accepted, a list of them is accepted.** So these say the same thing:

```echo
#[sources: "src/*.eco"]
#[sources: "gen/*.eco"]

#[sources: ["src/*.eco", "gen/*.eco"]]
```

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

A dependency can also say out loud what kind it is — `#[depends: path "../core"]` means exactly what the
bare string does. The tag exists so a second kind can arrive without the common case growing a word, and
`git { url: "...", rev: "..." }` is the shape that is waiting for one; today it parses, checks its fields
and is then refused, because nothing fetches a repository yet.

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

Artifacts live in `ecobuild` beside each manifest — everything the build produced, in one directory, so the
only thing left next to your source is the binary you asked for. A library's artifacts travel with the
library, deleting a checkout deletes them, and no two projects can be confused for one another.

A module can name its own directory instead:

```echo
#[build_dir: "target"]
```

which is relative to the manifest, like every other path in it. `--build-dir <dir>` overrides both, and puts
every module under one root with its own subdirectory — the manifest describes your project's layout, the
flag describes this particular build.

When you want to start over, `echoc clean` empties them:

```bash
$ echoc clean
[clean]
  stdlib  /home/you/.cache/echo/stdlib  kept (pass --stdlib to remove it)
  geom    /home/you/geom/ecobuild       removed
  app     /home/you/app/ecobuild        removed
removed 2 build directories.
```

It reaches the whole graph — a dependency's artifacts are as much your build's output as your own — and
`-n` shows you what it would do without doing it. It leaves the standard library's store alone by default,
since that one is shared by every project on the machine and is the slowest thing to build again.

The one thing it will not do is delete a directory it can't prove it made. Every build directory gets a
`CACHEDIR.TAG` — a small marker file that also tells backup tools to skip it — and if you point
`#[build_dir:]` or `--build-dir` at a directory that already holds something without one, the build refuses
before writing anything and `clean` refuses before removing anything. A build directory is a place that gets
emptied, and being sure about which one it is matters more than being convenient.

Two exceptions to "beside the manifest", both because a cache must never get in the way. The standard library
ships with the compiler rather than with your program, so it caches under `$XDG_CACHE_HOME/echo` (or
`~/.cache/echo`) instead of in the toolchain's own directory. And a store that can't be written — a read-only
library, an installed compiler, a full disk — isn't an error: the module is compiled and simply not kept,
reported as `(its cache directory is not writable)`.

What invalidates a module:

- its own sources or manifest
- any dependency
- any module compiled before it
- the build mode, whether it carries debug information, the target, or the compiler version

That last line matters more than it looks. An object built with assertions is not an object built without them,
so `--debug` and `--release` artifacts coexist rather than overwrite each other — and the same is true of `-g`
(see [debugging.md](debugging.md)), which puts a whole DWARF description into every object it touches.

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
