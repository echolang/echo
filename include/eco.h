#ifndef ECO_H
#define ECO_H

#pragma once

// ECHO VERSION
//
// the major and the minor are the hand-edited half, and this is the only place either is stated:
// editing one here *is* the decision to start a new series, and the commit that does it is an
// ordinary commit
#define ECO_VERSION_MAJOR 0
#define ECO_VERSION_MINOR 1

// the patch is a fact about the release history rather than about this source tree: every push to
// master is a patch release, and the number is whatever the highest v<major>.<minor>.<n> tag on the
// repository is, plus one. So CI resolves it and passes it in, and a series started by editing the
// minor above restarts at zero by construction, with nothing here to keep in step
//
// which is what the guard is for - the value below is only what a local build reports, and a second
// copy of the released number written here would be a copy that goes stale on every release
#ifndef ECO_VERSION_PATCH
#define ECO_VERSION_PATCH 0
#endif

// appended to the version verbatim, and empty for a release. CI stamps "-dev+<sha>" onto everything
// built off a branch, so a binary someone was handed can never be mistaken for a released one
#ifndef ECO_VERSION_SUFFIX
#define ECO_VERSION_SUFFIX ""
#endif

#define ECO_STRINGIFY_IMPL(x) #x
#define ECO_STRINGIFY(x) ECO_STRINGIFY_IMPL(x)

// what `echoc --version` prints, and the whole of that line - the release workflow string-compares it
// against the tag it resolved. Composed here rather than formatted at the call site
#define ECO_VERSION_STRING ECO_STRINGIFY(ECO_VERSION_MAJOR) "." ECO_STRINGIFY(ECO_VERSION_MINOR) "." ECO_STRINGIFY(ECO_VERSION_PATCH) ECO_VERSION_SUFFIX

// When set to "1", the standard library will be embedded into the "echoc" binary and loaded from there.
//
// Otherwise, the standard library will be loaded and recompiled from its
// source files on each run of "echoc". Keep in mind that this requires the stdlib folder to be
// at the absolute location of where "echoc" was built from, so this is only meant for local development.
//
// which is exactly why it is guarded: a *released* binary is built with "-DECO_EMBED_STDLIB=ON", because
// the absolute path above is the release machine's and means nothing on the machine that downloaded it.
// The default here stays "0" - a local build wants the stdlib it can edit, and the tests assert against
// the source tree
#ifndef ECO_USE_EMBEDDED_STDLIB
#define ECO_USE_EMBEDDED_STDLIB 0
#endif

// handy for debugging
// when set to 1, exceptions will not be caught and will crash the program
// allowing for easier debugging
#define ECO_DONT_CATCH_EXCEPTIONS 1

// the name of the main module
#define ECO_MAIN_MODULE_NAME "main"

// the symbol the entry module's file-scope statements are emitted under - the C `main` a linker or a
// JIT looks for. deliberately *not* ECO_MAIN_MODULE_NAME: that one names a module and is overridable
// by a manifest, this one is fixed by the platform. Three places must agree on it - the definition
// LLVMCompiler emits, the JIT lookup, and the root set Backend::prune_to_entry keeps - and a
// disagreement leaves an empty module rather than a diagnostic
#define ECO_ENTRY_SYMBOL_NAME "main"

// the width of a pointer on the target, in bytes. this is what `usize` and `isize` lower to,
// and it is the single place that knows it: AST::get_primitive_size answers from here and
// TypeLowering picks its llvm integer width from here, so the two can never disagree
//
// only 64-bit targets are wired up and tested today. this constant is why widening to a 32-bit
// target is one edit rather than an api break across the whole stdlib
#define ECO_TARGET_POINTER_SIZE 8

// the module cache's format version. **Bump this by hand whenever codegen changes what it emits.**
//
// The cache key folds in the LLVM version, the target triple and the build mode, so those are covered
// automatically - this constant covers the one input nothing can detect: a change to this compiler's own
// lowering. Forgetting it means a stale object silently linked into a new build, which is the single failure
// mode in the cache with no diagnostic
#define ECO_MODULE_CACHE_VERSION "26"

// the same knob for the C object cache, and a separate one because the two caches are separate stores
// keyed on unrelated inputs: a codegen change moves every Echo object and no C one, and a change to how
// `#[cc:]` builds a translation unit - a flag added, the language inference changed - moves every C object
// and no Echo one. Sharing a constant would rebuild the wrong half every time either moved
#define ECO_C_BUILD_VERSION "1"

#endif // ECO_H
