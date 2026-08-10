#ifndef HOSTTOOL_H
#define HOSTTOOL_H

#pragma once

#include <string>
#include <vector>

namespace Compiler
{
    // **the sole way echoc runs another program**, and the whole of what one costs a caller.
    //
    // **as an argv, with no shell in between.** An SDK path, an output name or a vendored source directory
    // may contain a space, and a quoting layer over std::system has no answer at all for one containing a
    // quote - which is what this replaced. A new tool's arguments go in as words, never as a command line.
    //
    // true when the program was found and exited zero. Its own stdout and stderr are inherited, so a
    // clang diagnostic reaches the terminal as clang wrote it rather than through a renderer that would
    // have to pretend it understood it
    bool run_tool(const std::vector<std::string> &argv);
};

#endif
