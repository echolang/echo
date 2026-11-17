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
    //
    // an inline copy folds apart from a definition: no object code comes of one, so it is not the
    // same partition as one that defines the body
    std::string partition_hex(
        llvm::Module &module,
        const std::unordered_set<const llvm::GlobalValue *> &needed,
        const std::function<Definition(const llvm::GlobalValue *)> &definition_of,
        const PartitionEnv &env
    );
};

#endif
