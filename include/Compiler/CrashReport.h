#ifndef CRASHREPORT_H
#define CRASHREPORT_H

#pragma once

namespace Compiler
{
    // **a compiler bug still crashes, it just no longer crashes mute.**
    //
    // ECO_DONT_CATCH_EXCEPTIONS lets an exception escape so a debugger stops where it was thrown, and
    // that stays true: the handlers installed here write one line to stderr and then abort exactly as
    // before. What they fix is Windows, where an escaped exception goes terminate -> abort ->
    // __fastfail and the process exits 0xC0000409 having printed nothing at all - the same status the
    // CRT's invalid-parameter handler ends in, so the two are covered together. Linux names the
    // exception on its own; a release log from Windows did not, and that is a crash nobody can read.
    //
    // called once, first thing in main
    void install_crash_reporting();
};

#endif
