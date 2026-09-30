#ifndef JOBCOUNT_H
#define JOBCOUNT_H

#pragma once

#include <cstddef>
#include <functional>

namespace Compiler
{
    // how many independent pieces of work this invocation should run at once.
    //
    // `ECO_JOBS` if it is a positive integer, otherwise `std::thread::hardware_concurrency()`,
    // and at least 1. C translation units and LLVM compilation units both read this, so a
    // serial run for a test is one environment variable rather than two flags that can drift
    unsigned job_count();

    // run `work(0)` .. `work(count - 1)`. one item, or `jobs <= 1`, stays on this thread so
    // `ECO_JOBS=1` cannot drift between C translation units and LLVM units
    void run_jobs(unsigned jobs, size_t count, const std::function<void(size_t)> &work);
};

#endif
