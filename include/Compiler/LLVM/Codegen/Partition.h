#ifndef PARTITION_H
#define PARTITION_H

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace AST
{
    class File;
};

namespace Compiler::LLVM
{
    struct CodegenContext;
    struct CmpUnit;

    // what a partition's object is a function of besides its bitcode. the same axes
    // Compiler::compute_module_keys folds for a whole-unit object, minus the sources:
    // the bitcode *is* the sources
    struct PartitionEnv
    {
        std::string triple;
        std::string cpu;
        std::string features;
        bool no_optimize = false;
        bool targeting_windows = false;
        bool cross = false;
    };

    struct UnitPartition
    {
        // the source file's basename, or `__shared` for foreign `linkonce_odr` and
        // anything `function_file_map` does not place. diagnostics only; the object
        // name is `hex`
        std::string label;
        std::string hex;
        bool shared = false;
        AST::File *file = nullptr;
        // names of the group's roots in the still-live unit module. `materialize_partition`
        // looks them up; empty after the module is dropped
        std::vector<std::string> root_names;
    };

    // split one unit into per-file groups. empty when the unit has nothing to split
    // (one partition, or none). each partition's `hex` is a structural hash of the
    // original IR that group will emit, plus `env` - not of a `CloneModule` bitcode
    // dump, whose metadata numbering moved across identical rebuilds
    //
    // a this-module function lands in that file's partition. `linkonce_odr` that
    // `function_file_map` places in another module - generic instantiations,
    // synthesized deinits - land in `__shared`, and a small `#[inline]` helper is
    // cloned into each partition that calls it, the C++ TU model. a call into a
    // large ODR body is an external from the file object and a definition in
    // `__shared`, so ISel of a huge deinit runs once
    std::vector<UnitPartition> partition_unit(
        CmpUnit &unit, const CodegenContext &ctx, const PartitionEnv &env);

    // extract only the globals this partition defines or references and serialize
    // them. called on a cache miss, while `unit.llvm_module` still lives
    std::string materialize_partition(
        CmpUnit &unit,
        const UnitPartition &part,
        const CodegenContext &ctx);

    // host `ld -r` can consume objects this env would emit. native Unix/Darwin only:
    // Windows has no `ld -r`, and a cross object's format is not the host linker's
    bool can_join_relocatable(const PartitionEnv &env);

    // `ld -r` the partition objects into one relocatable, so the rest of emission
    // still sees one object per unit. one input is a copy rather than a link
    bool join_relocatable(
        const std::vector<std::filesystem::path> &objects,
        const std::filesystem::path &output,
        const PartitionEnv &env,
        std::string &error);
};

#endif
