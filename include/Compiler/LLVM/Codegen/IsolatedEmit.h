#ifndef ISOLATEDEMIT_H
#define ISOLATEDEMIT_H

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace llvm
{
    class Module;
    class TargetMachine;
};

namespace Compiler::LLVM
{
    // one unit, in its own LLVMContext. the driver serialises the unit to bitcode because a
    // context is not safe to share, then a worker parses and runs the same prepare + emit path
    // `emit_object` uses. the object is a pure function of that bitcode plus this environment
    struct IsolatedEmitRequest
    {
        std::string bitcode;
        std::filesystem::path object_path;
        std::string triple;
        std::string cpu;
        std::string features;
        bool no_optimize = false;
        bool targeting_windows = false;
        bool already_optimized = false;
        bool timings = false;
    };

    struct IsolatedEmitResult
    {
        bool ok = true;
        std::string error;
        double optimize_ms = 0.0;
        double isel_ms = 0.0;
        std::vector<std::pair<std::string, double>> slowest_isel;
    };

    IsolatedEmitResult emit_isolated_unit(const IsolatedEmitRequest &request);

    bool emit_module_object(
        llvm::Module &module,
        llvm::TargetMachine &target_machine,
        const std::filesystem::path &object_path,
        bool targeting_windows,
        std::vector<std::pair<std::string, double>> *isel_times,
        std::string &error
    );
};

#endif
