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

// the width of a pointer on the target, in bytes. this is what `usize` and `isize` lower to,
// and it is the single place that knows it: AST::get_primitive_size answers from here and
// TypeLowering picks its llvm integer width from here, so the two can never disagree.
//
// only 64-bit targets are wired up and tested today. this constant is why widening to a 32-bit
// target is one edit rather than an api break across the whole stdlib
#define ECO_TARGET_POINTER_SIZE 8

#endif // ECO_H