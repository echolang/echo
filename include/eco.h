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

#endif // ECO_H