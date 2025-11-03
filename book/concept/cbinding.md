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

Both `echoc run` (which resolves symbols in the running process) and `echoc build` (which links with `clang`)
reach libc with no extra flags.
