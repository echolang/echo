#ifndef ECO_H
#define ECO_H

#pragma once

// When set to "1", the embedded standard library will be used, which is the default behavior.
//
// This means the standard library will be embedded into the "echoc" binary and will be loaded from there.
//
// Otherwise, the standard library will be loaded and recompiled from its 
// source files on each run of "echoc". Keep in mind that this requires the stdlib folder to be 
// at the absolute location of where "echoc" was built from, so this is only meant for local development.
#define ECO_USE_EMBEDDED_STDLIB 0

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
#define ECO_MODULE_CACHE_VERSION "1"

#endif // ECO_H