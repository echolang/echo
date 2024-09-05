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

A declaration inside the block is **bodyless** — it ends at the semicolon, because the body is
somebody else's. Its name reaches the linker exactly as written, with no mangling, which is the
whole point of the block.

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

This is how the standard library binds the C allocator — see `stdlib/mem/mem.eco`, where the whole
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

It is worth wrapping a C function rather than calling it everywhere, so the awkward parts stay in one
place:

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

## Rules and limits

- **No generics.** A single C symbol has no per-instantiation body, so
  `function malloc as alloc<T>(...)` is rejected. Write the generic in Echo over a concrete extern —
  that is exactly what `mem::alloc<T>` does.
- **No body.** A body in an extern block is rejected; it would be compiled under the raw symbol and
  collide with the real definition at link time.
- **One signature per symbol.** Declaring the same C symbol twice with different argument or return
  types is an error. Under LLVM's opaque pointers a mismatch is otherwise invisible in the IR, so
  the compiler compares the signatures rather than letting two incompatible call sequences share a
  name.
- **No variadics.** There is no spelling for `...`, so `printf` and friends cannot be declared yet.
  (The compiler declares `printf` itself for `echo`'s benefit.)
- **Types are yours to get right.** Nothing checks a declaration against the real C header. `usize`
  stands in for `size_t` and `int32` for C's `int`, which agree on every target Echo currently
  supports.

Both `echoc run` (which resolves symbols in the running process) and `echoc build` (which links with
`clang`) reach libc without any extra flags.
