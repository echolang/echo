#ifndef PARTITION_H
#define PARTITION_H

#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace AST
{
    class File;
};

namespace llvm
{
    class GlobalValue;
    class Module;
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
        // Compiler::exec_model_environment
        std::string exec_model;
    };

    // what a partition holds of a global it reaches
    enum class Definition
    {
        // a declaration: some other partition, or another object, defines it
        t_declared,
        // this partition's object defines it
        t_defined,
        // a body for this partition's inliner and no object code. the definition is its home partition's
        t_inline_copy,
    };

    struct UnitPartition
    {
        // the source file's basename, or `__shared` for foreign `linkonce_odr` and
        // anything `file_of` does not place. diagnostics only; the object
        // name is `hex`
        std::string label;
        std::string hex;
        bool shared = false;
        // partition_unit's `clone_inline`, kept so materialize_partition defines what `hex` hashed
        bool clone_inline = true;
        std::vector<AST::File *> files;
        // names of the group's roots in the module partition_unit was handed.
        // `materialize_partition` looks them up there
        std::vector<std::string> root_names;
    };

    // globals reachable from `roots`, including a GlobalVariable's initializer walked once.
    // partition_unit's worklist; tests pin that cyclic globals terminate
    std::unordered_set<const llvm::GlobalValue *> reachable_from(const std::vector<llvm::GlobalValue *> &roots);

    // split one unit into per-file groups. **`module` is the unit's IR and need not be the one the
    // unit holds**: a library is optimized in a worker's own LLVMContext, and it is that copy which is
    // split. `unit` answers by symbol name where a function was written, which reads nothing a worker
    // may not read beside another. empty when the unit has nothing to split
    // (one partition, or none). each partition's `hex` is a structural hash of the
    // original IR that group will emit, plus `env` - not of a `CloneModule` bitcode
    // dump, whose metadata numbering moved across identical rebuilds
    //
    // a this-module function lands in that file's partition. `linkonce_odr` that
    // `file_of` places in another module - generic instantiations,
    // synthesized deinits - land in `__shared`. **every body has that one home**, so
    // ISel of it runs once however many files call it
    //
    // `clone_inline` is whether a pass that inlines still runs over the pieces. when one does, a
    // small `#[inline]` helper is also carried by each partition that calls it, as a body the
    // inliner may read and the object never holds. when none does - the unit was optimized whole
    // before the split, or nothing is optimized at all - there is nobody to read it
    //
    // `max_groups` > 0 packs the file/`__shared` groups into that many bins after the split.
    // a cap on ISel machines, not a load-balance: packing files around `__shared` does not
    // shrink the shared object if that is what actually takes the time
    std::vector<UnitPartition> partition_unit(
        CmpUnit &unit,
        llvm::Module &module,
        const CodegenContext &ctx,
        const PartitionEnv &env,
        size_t max_groups = 0,
        bool clone_inline = true);

    // extract only the globals this partition defines or references and serialize
    // them. called on a cache miss, with the module partition_unit split
    std::string materialize_partition(
        CmpUnit &unit,
        llvm::Module &module,
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
