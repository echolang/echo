#ifndef FUNCTIONTIMETRACE_H
#define FUNCTIONTIMETRACE_H

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Compiler::LLVM
{
    // per-function times for `--explain time`, read off LLVM's own time-trace: only the pass
    // manager knows which function is current. a legacy `OptFunction` is taken whole; new-PM
    // pass events nest, so they are self times. module-level time is left out.
    // scoped to the thread, like the profiler
    class FunctionTimeTrace
    {
    public:
        // no `out`, no trace
        explicit FunctionTimeTrace(std::vector<std::pair<std::string, double>> *out);
        explicit FunctionTimeTrace(std::string_view kind);
        ~FunctionTimeTrace();

        FunctionTimeTrace(const FunctionTimeTrace &) = delete;
        FunctionTimeTrace &operator=(const FunctionTimeTrace &) = delete;

    private:
        std::vector<std::pair<std::string, double>> *_out = nullptr;
        std::vector<std::pair<std::string, double>> _recorded;
        std::string _kind;
        bool _live = false;
    };
};

#endif
