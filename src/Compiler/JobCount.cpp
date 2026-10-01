#include "Compiler/JobCount.h"

#include <llvm/Support/ThreadPool.h>
#include <llvm/Support/Threading.h>

#include <cstdlib>
#include <thread>

unsigned Compiler::job_count()
{
    if (const char *spelled = std::getenv("ECO_JOBS")) {
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(spelled, &end, 10);

        if (end != spelled && *end == '\0' && parsed >= 1 && parsed <= 1024) {
            return static_cast<unsigned>(parsed);
        }
    }

    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

void Compiler::run_jobs(unsigned jobs, size_t count, const std::function<void(size_t)> &work)
{
    if (count == 0) {
        return;
    }

    if (count == 1 || jobs <= 1) {
        for (size_t i = 0; i < count; i++) {
            work(i);
        }

        return;
    }

    llvm::DefaultThreadPool pool(llvm::hardware_concurrency(jobs));

    for (size_t i = 0; i < count; i++) {
        pool.async([&work, i] {
            work(i);
        });
    }

    pool.wait();
}
