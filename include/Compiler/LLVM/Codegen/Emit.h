#ifndef EMIT_H
#define EMIT_H

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Compiler::LLVM
{
    struct CodegenContext;
    class Backend;

    // unit -> object(s): partition, isolate, pool, join. LLVMCompiler::emit_objects forwards here
    bool emit_unit_objects(
        CodegenContext &ctx,
        Backend &backend,
        const std::function<std::filesystem::path(const std::string &)> &object_for,
        std::vector<std::filesystem::path> &out_objects
    );
};

#endif
