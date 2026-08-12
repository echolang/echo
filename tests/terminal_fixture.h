#ifndef TESTS_TERMINAL_FIXTURE_H
#define TESTS_TERMINAL_FIXTURE_H

#pragma once

#include <Compiler/TerminalCapabilities.h>

namespace EchoTests
{
    // a terminal, as far as anything that draws on one is concerned.
    //
    // **one signature, because three suites assert on the same facts.** The help page, the progress
    // checklist and the test reporter each build these, and while each had its own fixture the three
    // disagreed about what the positions meant - `a_terminal(true, true)` was "unicode and colour" in one
    // and "unicode at a width of one" in another, which is a fixture that lies rather than one that
    // duplicates.
    //
    // colour is off by default so an assertion reads as the row rather than as a sequence of escapes
    inline Compiler::TerminalCapabilities a_terminal(
        bool unicode = true,
        unsigned int width = 80,
        bool color = false
    )
    {
        Compiler::TerminalCapabilities capabilities;
        capabilities.color = color;
        capabilities.unicode = unicode;
        capabilities.width = width;
        capabilities.interactive = true;

        return capabilities;
    }
};

#endif
