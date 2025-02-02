#ifndef COMPILEROPTIONS_H
#define COMPILEROPTIONS_H

#pragma once

namespace Compiler
{
    // how much the compiler owes the running program in checks it could skip
    //
    // this is a property of *the program being compiled*, not of how echoc itself was built. the
    // distinction is the whole reason the type exists: the null-narrowing check used to be gated on
    // the host compiler's NDEBUG, which meant a release build of a user's program still carried
    // every check as long as echoc had been built with assertions on
    enum class BuildMode
    {
        t_debug,
        t_release,
    };

    struct CompilerOptions
    {
        BuildMode mode = BuildMode::t_debug;

        // one predicate, because more than one emitter asks it - the `assert` builtin and the
        // `ptr<T>` -> `T&` narrowing today, whatever check comes next tomorrow. never compare the
        // enum at a call site, or the next check added answers the question its own way
        bool assertions_enabled() const {
            return mode == BuildMode::t_debug;
        }
    };
};

#endif
