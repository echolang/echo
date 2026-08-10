# C Binding

Echo can call C functions directly. Declare them in an `extern { }` block: the block says "these
live in another object file, link against them by name."

```echo
extern {
    function time(ptr<uint64> $out) : int64;
}

uint64 $now;
time(&$now);
echo $now;
```

A declaration inside the block is **bodyless** — it ends at the semicolon, because the body is somebody else's.
Its name reaches the linker exactly as written, with no mangling, which is the whole point of the block.

Almost everything the block refuses follows from that one sentence. You can't give a declaration a body, because
it would be compiled under the raw symbol and collide with the real definition at link time. You can't make one
generic — `function malloc as alloc<T>(...)` is rejected, because a single C symbol has no per-instantiation
body to be generic *over*. Write the generic in Echo on top of a concrete extern instead, which is exactly what
`mem::alloc<T>` does. And you can only declare a given C symbol once: two declarations with different argument
or return types are an error rather than a coin flip at the call site.

One thing that genuinely isn't supported yet: **variadics.** There's no spelling for `...`, so `printf` and
friends can't be declared.

## Renaming

C names are often terser or more collision-prone than you want in Echo. `as` gives the symbol a
different name for callers:

```echo
extern {
    function malloc as alloc_bytes(usize $bytes) : ptr<uint8>;
    function free as free_bytes(ptr<uint8> $p) : void;
}
```

The first identifier is the **C symbol** and only the linker sees it; the name after `as` is what
Echo code uses. Without `as`, the two are the same.

This is how the standard library binds the C allocator — see `stdlib/core/mem.eco`, where the whole
`mem::` surface is Echo code layered over five extern declarations.

## Namespacing

An extern block obeys the enclosing namespace like any other declaration, so you can wrap a C
library without its names leaking:

```echo
namespace posix;

extern {
    function getpid() : int32;
}
```

Callers then write `posix::getpid()`. Note that the *Echo* name is namespaced while the symbol stays
`getpid` — namespacing costs nothing at the ABI level.

## Wrapping

It's worth wrapping a C function rather than calling it everywhere, so the awkward parts stay in one place:

```echo
namespace time;

extern {
    function time as c_time(ptr<uint64> $out) : int64;
}

function now() : uint64
{
    uint64 $seconds;
    c_time(&$seconds);
    return $seconds;
}
```

Wrapping is also where you absorb the one thing nothing can check for you: **the types are yours to get
right.** Nothing compares your declaration against the real C header — there's no header to compare against —
so a wrong signature is a wrong signature all the way down to the call. `usize` stands in for `size_t` and
`int32` for C's `int`, which agree on every target Echo currently supports. Get it right once, in the wrapper,
and the rest of your program can't get it wrong.

## Strings and C

**A string literal is not a `ptr<const uint8>`.** It's a `string`, and there's no implicit conversion between
the two. That's deliberate: a `string` carries a length and a raw pointer doesn't, so decaying one would
silently drop it and hand C a pointer with nothing promising a terminator behind it.

So you write the conversion out, with `->c_str()`:

```echo
extern { function puts(ptr<const uint8> $s) : int32; }

$msg = 'hello';
puts($msg->c_str());   // hello
```

That leaves exactly one place in your source — the call itself — where a string becomes a raw address, which is
where you want it.

`c_str()` costs nothing, because every string is allocated with room for a trailing `0` and every literal is
emitted terminated. What it does *not* do is protect you from a substring: `c_str()` on a view whose window
stops short of the terminator asserts rather than handing C a pointer that runs off the end. `->clone()` first
when you need a terminated copy, or ask `is_terminated()` when you'd rather branch than die.

And a `0` byte *inside* the string still ends the C string early. That's what a C string is, and nothing here
can help you with it.

## Linking

libc needs nothing. Both `echoc run` and `echoc build` reach it with no flags at all, which is why every
example above compiles as written.

Anything else has to be linked, and **the module that needs it is what says so** — in its `module.eco`, with
`#[link:]`:

```echo
#[module: "opengl"]
#[version: "0.1.0"]

#[link: lib "glfw"]

#[sources: "src/*.eco"]
```

A program depending on that module writes `#[depends: "../opengl"]` and nothing else. It doesn't repeat the
library, and it doesn't need to know there is one. That's the whole point: which libraries a binding needs is
the binding's business, and a requirement travels with it through as many modules as sit in between.

### The four kinds

Every value is *tagged* with what kind of thing it names, and the list is closed:

| | |
|---|---|
| `lib "GL"` | a library by name, resolved the way the linker resolves everything else |
| `framework "OpenGL"` | a Darwin framework. Refused anywhere else — see below |
| `search "vendor/lib"` | a directory to look in, **relative to the manifest** |
| `object "vendor/glad.o"` | a prebuilt object file, also relative to the manifest |

The tag isn't optional, and `#[link: "GL"]` is an error rather than shorthand. That's deliberate: if a bare
string meant "a library", then a misspelled `framwork "OpenGL"` would have to mean *something*, and you'd
find out at link time in a message about a library you never wrote. With the tag required, the typo is
caught in the manifest, on the line that has it — and pointing at the word that is wrong.

Naming several at once is fine, and means exactly what writing them separately means:

```echo
#[link: lib ["GL", "GLU"]]
#[link: [lib "GL", framework "OpenGL"]]
```

### Platforms

A `module.eco` is Echo, read through the same conditional filter every other file goes through, so gating a
link requirement needs no new syntax:

```echo
#[if: os == darwin]
#[link: framework ["OpenGL", "Cocoa"]]
#[elif: os == linux]
#[link: lib "GL"]
#[end]
```

The same `#[if:]` gates the `extern` blocks in your sources, and both are evaluated against the same facts —
so you can't end up with one platform's declarations and another's libraries.

A `framework` outside Darwin is refused outright rather than ignored. Ignoring it would leave a Linux build
failing on exactly the symbols the declaration was supposed to provide, with nothing saying it had been
dropped — so the compiler asks you to write the `#[if:]`, once.

### When a library is somewhere odd

Where a library is *installed* is a property of the machine, not of your program, so it doesn't belong in a
manifest. `--link` names the same four kinds, on either subcommand — with a colon rather than a space,
because a shell would otherwise want the quotes escaped:

```bash
echoc build --link search:/opt/homebrew/lib -o app
echoc run   --link search:/opt/homebrew/lib
```

These merge after every manifest's, so they only ever add — a declaration always wins.

## Compiling C alongside Echo

Sometimes a binding needs actual C: a loader like glad, a `static inline` header wrapper, a platform entry
point. `#[cc:]` compiles it and puts the objects in your link.

```echo
#[cc: sources "c/*.c"]
#[cc: include "c/include"]
#[cc: define "GLAD_GL_IMPLEMENTATION"]
#[cc: flag "-Wno-unused-function"]
```

Four kinds, tagged the same way as `#[link:]`'s. `sources` is a pattern, expanded exactly like
`#[sources:]`; `include` is a directory relative to the manifest; `define` and `flag` go to the C compiler
as written. Language is picked by extension, so `.c`, `.m` and `.cpp` all work — a C++ shim just adds
`#[link: lib "c++"]` itself.

`define` is the one that takes a **record**, because its keys are your macro names rather than a vocabulary
the compiler knows. All three of these spellings mean the same thing:

```echo
#[cc: define "NDEBUG"]                                     // -DNDEBUG
#[cc: define ["NDEBUG", "TRACE"]]                          // -DNDEBUG -DTRACE
#[cc: define { GLAD_GL_IMPLEMENTATION: 1, WIDTH: 40 }]     // -DGLAD_GL_IMPLEMENTATION=1 -DWIDTH=40
```

Nothing is re-quoted on the way out — the C compiler is run with an argument list and no shell in between,
so a define holding a space arrives holding a space.

**The C contributes objects and nothing else.** No include path and no macro from a `#[cc:]` reaches Echo's
front end. Echo does not read C headers, so your `extern { }` declarations are still written by hand and
still yours to get right — which is the same deal as everywhere else on this page, and the reason the
wrapping advice above matters.

Objects are cached beside the module, keyed on the source *and every header it included* — clang reports
those and echoc reads them back, so editing a header rebuilds what needs rebuilding and nothing else.

Under `echoc run` there's no link at all, so the compiler builds those objects into a loadable library and
opens it before the program starts. You don't have to do anything; it's worth knowing only because it's the
reason `run` works on a program with a C shim in it.

[examples/opengl_triangle](../../examples/opengl_triangle) is all of this in one small program.
