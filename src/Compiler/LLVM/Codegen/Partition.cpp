#include "Compiler/LLVM/Codegen/Partition.h"

#include "Compiler/LLVM/Codegen/Bitcode.h"
#include "Compiler/LLVM/Codegen/PartitionHash.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/ModuleCache.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/FunctionDeclNode.h"
#include "eco.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalIFunc.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <fmt/core.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

constexpr unsigned k_inline_instruction_limit = 64;

bool is_inline_worthy(const llvm::Function &function)
{
    if (function.isDeclaration()) {
        return false;
    }

    if (function.hasFnAttribute(llvm::Attribute::InlineHint)
        || function.hasFnAttribute(llvm::Attribute::AlwaysInline)) {
        return true;
    }

    unsigned count = 0;

    for (const llvm::BasicBlock &block : function) {
        count += block.size();
        if (count > k_inline_instruction_limit) {
            return false;
        }
    }

    return count > 0;
}

bool is_odr_linkage(const llvm::GlobalValue &global)
{
    return global.hasLinkOnceODRLinkage() || global.hasWeakODRLinkage();
}

bool is_llvm_global_name(llvm::StringRef name)
{
    return name.starts_with("llvm.");
}

void consider_value(
    llvm::Value *value,
    std::unordered_set<const llvm::GlobalValue *> &needed,
    std::vector<llvm::GlobalValue *> &work
)
{
    auto *global = llvm::dyn_cast_or_null<llvm::GlobalValue>(value);
    if (global == nullptr) {
        return;
    }

    if (needed.insert(global).second) {
        work.push_back(global);
    }
}

void walk_constant(
    llvm::Constant *constant,
    std::unordered_set<const llvm::GlobalValue *> &needed,
    std::vector<llvm::GlobalValue *> &work
)
{
    if (constant == nullptr) {
        return;
    }

    consider_value(constant, needed, work);

    for (llvm::Value *operand : constant->operand_values()) {
        if (auto *inner = llvm::dyn_cast<llvm::Constant>(operand)) {
            walk_constant(inner, needed, work);
        }
    }
}

void walk_metadata(
    const llvm::Metadata *metadata,
    std::unordered_set<const llvm::GlobalValue *> &needed,
    std::vector<llvm::GlobalValue *> &work,
    llvm::DenseSet<const llvm::Metadata *> &seen
)
{
    if (metadata == nullptr || !seen.insert(metadata).second) {
        return;
    }

    if (auto *value = llvm::dyn_cast<llvm::ValueAsMetadata>(metadata)) {
        if (auto *constant = llvm::dyn_cast<llvm::Constant>(value->getValue())) {
            walk_constant(constant, needed, work);
        }

        return;
    }

    if (auto *node = llvm::dyn_cast<llvm::MDNode>(metadata)) {
        for (const llvm::MDOperand &operand : node->operands()) {
            walk_metadata(operand.get(), needed, work, seen);
        }
    }
}

std::unordered_set<const llvm::GlobalValue *> reachable_from(
    const std::vector<llvm::GlobalValue *> &roots
)
{
    std::unordered_set<const llvm::GlobalValue *> needed;
    std::vector<llvm::GlobalValue *> work;

    for (llvm::GlobalValue *global : roots) {
        if (needed.insert(global).second) {
            work.push_back(global);
        }
    }

    while (!work.empty()) {
        llvm::GlobalValue *global = work.back();
        work.pop_back();

        if (auto *function = llvm::dyn_cast<llvm::Function>(global)) {
            if (function->isDeclaration()) {
                continue;
            }

            if (function->hasPersonalityFn()) {
                walk_constant(function->getPersonalityFn(), needed, work);
            }

            if (function->hasPrefixData()) {
                walk_constant(function->getPrefixData(), needed, work);
            }

            if (function->hasPrologueData()) {
                walk_constant(function->getPrologueData(), needed, work);
            }

            llvm::DenseSet<const llvm::Metadata *> md_seen;

            for (llvm::BasicBlock &block : *function) {
                for (llvm::Instruction &instruction : block) {
                    for (llvm::Value *operand : instruction.operand_values()) {
                        if (auto *constant = llvm::dyn_cast<llvm::Constant>(operand)) {
                            walk_constant(constant, needed, work);
                        }
                        else {
                            consider_value(operand, needed, work);
                        }
                    }

                    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> attached;
                    instruction.getAllMetadata(attached);

                    for (const auto &item : attached) {
                        walk_metadata(item.second, needed, work, md_seen);
                    }
                }
            }
        }
        else if (auto *variable = llvm::dyn_cast<llvm::GlobalVariable>(global)) {
            if (variable->hasInitializer()) {
                walk_constant(variable->getInitializer(), needed, work);
            }
        }
        else if (auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(global)) {
            walk_constant(alias->getAliasee(), needed, work);
        }
        else if (auto *ifunc = llvm::dyn_cast<llvm::GlobalIFunc>(global)) {
            if (ifunc->getResolver()) {
                walk_constant(ifunc->getResolver(), needed, work);
            }
        }
    }

    return needed;
}

void canonicalize_unnamed_locals(llvm::Module &module)
{
    for (llvm::GlobalVariable &global : module.globals()) {
        if (!global.hasLocalLinkage() || global.hasName()) {
            continue;
        }

        std::string printed;
        llvm::raw_string_ostream stream(printed);

        if (global.hasInitializer()) {
            global.getInitializer()->print(stream);
        }

        stream.flush();
        const uint64_t digest = Compiler::fnv1a64(printed, Compiler::k_fnv_offset_basis);
        global.setName(fmt::format("g.{}", Compiler::to_hex(digest)));
    }
}

void drop_unused_declarations(llvm::Module &module)
{
    bool changed = true;

    while (changed) {
        changed = false;
        std::vector<llvm::GlobalValue *> drop;

        for (llvm::Function &function : module) {
            if (function.isDeclaration() && function.use_empty()) {
                drop.push_back(&function);
            }
        }

        for (llvm::GlobalVariable &global : module.globals()) {
            if (global.isDeclaration() && global.use_empty() && !is_llvm_global_name(global.getName())) {
                drop.push_back(&global);
            }
        }

        for (llvm::GlobalValue *global : drop) {
            global->eraseFromParent();
            changed = true;
        }
    }
}

void keep_odr_from_dce(llvm::Module &module)
{
    // GlobalDCE drops unreferenced `linkonce_odr`. pin them in `llvm.compiler.used`
    // so a this-module copy constructor whose only calls sit in another file survives
    llvm::SmallVector<llvm::GlobalValue *, 8> keep;

    auto consider = [&](llvm::GlobalValue &global) {
        if (global.isDeclaration() || global.hasLocalLinkage() || global.hasExternalLinkage()) {
            return;
        }

        if (is_llvm_global_name(global.getName())) {
            return;
        }

        keep.push_back(&global);
    };

    for (llvm::Function &function : module) {
        consider(function);
    }

    for (llvm::GlobalVariable &global : module.globals()) {
        consider(global);
    }

    for (llvm::GlobalAlias &alias : module.aliases()) {
        consider(alias);
    }

    if (!keep.empty()) {
        llvm::appendToCompilerUsed(module, keep);
    }
}

void keep_module_flags_only(llvm::Module &module)
{
    std::vector<llvm::NamedMDNode *> drop;

    for (llvm::NamedMDNode &named : module.named_metadata()) {
        if (named.getName() != "llvm.module.flags") {
            drop.push_back(&named);
        }
    }

    for (llvm::NamedMDNode *named : drop) {
        named->eraseFromParent();
    }
}

std::unordered_map<const llvm::Function *, AST::File *> function_homes(
    Compiler::LLVM::CmpUnit &unit,
    const Compiler::LLVM::CodegenContext &ctx
)
{
    std::unordered_map<const llvm::Function *, AST::File *> home_of;

    if (unit.llvm_module == nullptr || unit.ast_module == nullptr) {
        return home_of;
    }

    llvm::Module &module = *unit.llvm_module;
    const Compiler::LLVM::FunctionTable &table = unit.function_table;

    AST::File *entry_home = nullptr;

    for (AST::File &file : unit.ast_module->files()) {
        if (!ctx.entry_file.empty() && file.get_path() == ctx.entry_file) {
            entry_home = &file;
            break;
        }
    }

    if (entry_home == nullptr) {
        entry_home = unit.ast_module->files().first();
    }

    for (llvm::Function &function : module) {
        if (function.isDeclaration()) {
            continue;
        }

        if (entry_home != nullptr && function.getName() == ECO_ENTRY_SYMBOL_NAME) {
            home_of[&function] = entry_home;
            continue;
        }

        const Compiler::LLVM::function_id_t id = table.get_function_id_by_name(std::string(function.getName()));
        if (id == 0) {
            continue;
        }

        const AST::FunctionDeclNode *decl = table.get_function(id).ast_funcdecl;
        if (decl == nullptr) {
            continue;
        }

        AST::File *file = ctx.file_of(decl);
        if (file == nullptr || file->module != unit.ast_module) {
            continue;
        }

        home_of[&function] = file;
    }

    return home_of;
}

bool should_define(
    const llvm::GlobalValue *global,
    const std::unordered_set<const llvm::GlobalValue *> &needed,
    const std::unordered_map<const llvm::Function *, AST::File *> &home_of,
    AST::File *group_file,
    bool group_shared
)
{
    if (needed.find(global) == needed.end()) {
        return false;
    }

    if (global->hasLocalLinkage()) {
        return true;
    }

    auto *function = llvm::dyn_cast<llvm::Function>(global);
    AST::File *home = nullptr;

    if (function != nullptr) {
        auto found = home_of.find(function);
        if (found != home_of.end()) {
            home = found->second;
        }
    }

    if (home == group_file) {
        return true;
    }

    if (function != nullptr && is_odr_linkage(*function) && is_inline_worthy(*function)) {
        return true;
    }

    if (function == nullptr && is_odr_linkage(*global)) {
        return true;
    }

    if (group_shared && (is_odr_linkage(*global) || function == nullptr)) {
        return true;
    }

    return false;
}

std::vector<llvm::GlobalValue *> roots_from_names(
    llvm::Module &module,
    const std::vector<std::string> &names
)
{
    std::vector<llvm::GlobalValue *> roots;
    roots.reserve(names.size());

    for (const std::string &name : names) {
        if (llvm::GlobalValue *global = module.getNamedValue(name)) {
            roots.push_back(global);
        }
    }

    return roots;
}

};

std::vector<Compiler::LLVM::UnitPartition> Compiler::LLVM::partition_unit(
    CmpUnit &unit,
    const CodegenContext &ctx,
    const PartitionEnv &env
)
{
    if (unit.llvm_module == nullptr || unit.ast_module == nullptr) {
        return {};
    }

    llvm::Module &module = *unit.llvm_module;
    const std::unordered_map<const llvm::Function *, AST::File *> home_of =
        function_homes(unit, ctx);

    std::unordered_map<AST::File *, std::vector<llvm::GlobalValue *>> roots_of;
    std::vector<llvm::GlobalValue *> shared_roots;

    for (llvm::Function &function : module) {
        if (function.isDeclaration()) {
            continue;
        }

        auto found = home_of.find(&function);
        if (found != home_of.end() && found->second != nullptr) {
            roots_of[found->second].push_back(&function);
        }
        else {
            shared_roots.push_back(&function);
        }
    }

    for (llvm::GlobalVariable &global : module.globals()) {
        if (global.isDeclaration() || global.hasLocalLinkage() || is_llvm_global_name(global.getName())) {
            continue;
        }

        shared_roots.push_back(&global);
    }

    for (llvm::GlobalAlias &alias : module.aliases()) {
        if (alias.hasLocalLinkage()) {
            continue;
        }

        shared_roots.push_back(&alias);
    }

    struct Group
    {
        std::string label;
        AST::File *file = nullptr;
        std::vector<llvm::GlobalValue *> roots;
        bool shared = false;
    };

    std::vector<Group> groups;

    for (AST::File &file : unit.ast_module->files()) {
        auto found = roots_of.find(&file);
        if (found == roots_of.end() || found->second.empty()) {
            continue;
        }

        groups.push_back(Group{
            file.get_path().filename().string(),
            &file,
            found->second,
            false,
        });
    }

    if (!shared_roots.empty()) {
        groups.push_back(Group{ "__shared", nullptr, shared_roots, true });
    }

    if (groups.size() < 2) {
        return {};
    }

    std::vector<UnitPartition> partitions;
    partitions.reserve(groups.size());

    for (Group &group : groups) {
        const std::unordered_set<const llvm::GlobalValue *> needed = reachable_from(group.roots);
        auto define = [&](const llvm::GlobalValue *global) -> bool {
            return should_define(global, needed, home_of, group.file, group.shared);
        };

        UnitPartition partition;
        partition.label = group.label;
        partition.shared = group.shared;
        partition.file = group.file;
        partition.hex = partition_hex(module, needed, define, env);

        for (llvm::GlobalValue *root : group.roots) {
            if (root->hasName()) {
                partition.root_names.push_back(std::string(root->getName()));
            }
        }

        partitions.push_back(std::move(partition));
    }

    return partitions;
}

std::string Compiler::LLVM::materialize_partition(
    CmpUnit &unit,
    const UnitPartition &part,
    const CodegenContext &ctx
)
{
    llvm::Module &module = *unit.llvm_module;
    const std::unordered_map<const llvm::Function *, AST::File *> home_of =
        function_homes(unit, ctx);

    const std::vector<llvm::GlobalValue *> roots = roots_from_names(module, part.root_names);
    const std::unordered_set<const llvm::GlobalValue *> needed = reachable_from(roots);

    llvm::ValueToValueMapTy vmap;
    std::unique_ptr<llvm::Module> clone = llvm::CloneModule(
        module, vmap, [&](const llvm::GlobalValue *global) {
            return should_define(global, needed, home_of, part.file, part.shared);
        });

    keep_odr_from_dce(*clone);
    keep_module_flags_only(*clone);
    drop_unused_declarations(*clone);
    canonicalize_unnamed_locals(*clone);

    return Compiler::LLVM::bitcode_of(*clone);
}
