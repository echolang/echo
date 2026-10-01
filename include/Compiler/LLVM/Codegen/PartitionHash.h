#ifndef PARTITIONHASH_H
#define PARTITIONHASH_H

#pragma once

#include "Compiler/LLVM/Codegen/Partition.h"

#include <functional>
#include <string>
#include <unordered_set>

namespace llvm
{
    class GlobalValue;
    class Module;
};

namespace Compiler::LLVM
{
    // structural hash of the IR a partition will emit, plus `env`. cache identity, so a
    // CloneModule bitcode dump whose metadata numbering moved is not a miss
    std::string partition_hex(
        llvm::Module &module,
        const std::unordered_set<const llvm::GlobalValue *> &needed,
        const std::function<bool(const llvm::GlobalValue *)> &define,
        const PartitionEnv &env
    );
};

#endif
