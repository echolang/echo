#ifndef TARGETMACHINE_H
#define TARGETMACHINE_H

#pragma once

#include <memory>
#include <string>

namespace llvm
{
    class TargetMachine;
};

namespace Compiler::LLVM
{
    std::unique_ptr<llvm::TargetMachine> make_target_machine(
        const std::string &triple,
        const std::string &cpu,
        const std::string &features,
        bool no_optimize,
        std::string &error
    );
};

#endif
