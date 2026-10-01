#include "Compiler/LLVM/Codegen/Partition.h"

#include "Compiler/HostTool.h"

#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>

#include <filesystem>
#include <string>
#include <vector>

bool Compiler::LLVM::can_join_relocatable(const PartitionEnv &env)
{
#if defined(_WIN32)
    (void)env;
    return false;
#else
    return !env.cross;
#endif
}

bool Compiler::LLVM::join_relocatable(
    const std::vector<std::filesystem::path> &objects,
    const std::filesystem::path &output,
    const PartitionEnv &env,
    std::string &error
)
{
    if (objects.empty()) {
        error = "nothing to join";
        return false;
    }

    if (objects.size() == 1) {
        std::error_code ec;
        std::filesystem::copy_file(
            objects[0], output, std::filesystem::copy_options::overwrite_existing, ec);

        if (ec) {
            error = fmt::format("could not copy '{}': {}", objects[0].string(), ec.message());
            return false;
        }

        return true;
    }

    std::vector<std::string> argv = { "ld", "-r" };

#if defined(__APPLE__)
    const std::string arch = llvm::Triple(env.triple).getArchName().str();

    if (arch.empty()) {
        error = "ld -r needs a target architecture";
        return false;
    }

    argv.push_back("-arch");
    argv.push_back(arch);
#endif

    argv.push_back("-o");
    argv.push_back(output.string());

    for (const std::filesystem::path &object : objects) {
        argv.push_back(object.string());
    }

    if (!Compiler::run_tool(argv)) {
        error = "ld -r failed";
        return false;
    }

    return true;
}
