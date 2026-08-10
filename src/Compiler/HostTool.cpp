#include "Compiler/HostTool.h"

#include "Compiler/ProgressReporter.h"

#include <llvm/Support/Program.h>

bool Compiler::run_tool(const std::vector<std::string> &argv)
{
    if (argv.empty()) {
        return false;
    }

    llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName(argv.front());

    if (!program) {
        return false;
    }

    std::vector<llvm::StringRef> args(argv.begin(), argv.end());

    // the resolved path rather than the name, so the child sees the same program findProgramByName picked
    args.front() = program.get();

    // **the obligation that comes with inheriting the streams.** The child is about to write into the
    // same stderr a progress row may be sitting on, and there is no lock either side can take because the
    // child is another process. Discharged here rather than at each call site for the reason the header
    // states the inheritance: it is this function's fact, so it is this function's consequence - and one
    // line here covers clang, ld, dsymutil and whatever is added next.
    //
    // sticky, so there is no matching call afterwards to forget: the next row this compile draws restores
    // itself. That is also what leaves the `llvm::errs()` notes right after a failed link needing no site
    // of their own
    ProgressReporter::instance().suspend();

    return llvm::sys::ExecuteAndWait(program.get(), args) == 0;
}
