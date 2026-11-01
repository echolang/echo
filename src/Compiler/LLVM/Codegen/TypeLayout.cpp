#include "Compiler/LLVM/Codegen/TypeLayout.h"

#include "AST/ASTCompleteness.h"
#include "AST/ASTValueType.h"
#include "Compiler/CompilerException.h"
#include "Compiler/LLVM/Codegen/StructureLayout.h"
#include "Compiler/LLVM/Codegen/TargetMachine.h"
#include "Compiler/TargetSubtarget.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Target/TargetMachine.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace
{
    struct LayoutState
    {
        llvm::LLVMContext context;
        llvm::DataLayout layout;
        std::unordered_map<const AST::ComplexType *, llvm::Type *> structs;
        LayoutState *previous = nullptr;

        explicit LayoutState(llvm::DataLayout layout)
            : layout(std::move(layout))
        {}

        llvm::Type *llvm_type(const AST::ValueType &type);
        llvm::Type *struct_type(const AST::ComplexType *ct);
    };

    LayoutState *t_current = nullptr;

    std::optional<uint64_t> query_size(const AST::ValueType &type)
    {
        if (t_current == nullptr) {
            return std::nullopt;
        }

        if (AST::type_completeness(type) != AST::TypeCompleteness::t_complete) {
            return std::nullopt;
        }

        llvm::Type *llvm_ty = t_current->llvm_type(type);
        if (llvm_ty == nullptr || llvm_ty->isVoidTy()) {
            return std::nullopt;
        }

        return t_current->layout.getTypeAllocSize(llvm_ty);
    }

    std::optional<uint64_t> query_align(const AST::ValueType &type)
    {
        if (t_current == nullptr) {
            return std::nullopt;
        }

        if (AST::type_completeness(type) != AST::TypeCompleteness::t_complete) {
            return std::nullopt;
        }

        llvm::Type *llvm_ty = t_current->llvm_type(type);
        if (llvm_ty == nullptr || llvm_ty->isVoidTy()) {
            return std::nullopt;
        }

        return t_current->layout.getABITypeAlign(llvm_ty).value();
    }
}

llvm::Type *LayoutState::struct_type(const AST::ComplexType *ct)
{
    if (ct == nullptr) {
        return nullptr;
    }

    auto found = structs.find(ct);
    if (found != structs.end()) {
        return found->second;
    }

    // opaque named placeholder so a field that points at this type can intern
    llvm::StructType *st = llvm::StructType::create(context);
    structs[ct] = st;

    if (!Compiler::LLVM::fill_aggregate_llvm_type(
            st, *ct, context, layout, [this](const AST::ValueType &field) {
                return llvm_type(field);
            })) {
        structs.erase(ct);
        return nullptr;
    }

    return st;
}

llvm::Type *LayoutState::llvm_type(const AST::ValueType &type)
{
    return Compiler::LLVM::llvm_type_of(
        type, context, layout, [this](const AST::ComplexType *ct) {
            return struct_type(ct);
        });
}

struct Compiler::LLVM::LayoutQueryScope::Impl
{
    std::unique_ptr<llvm::TargetMachine> machine;
    std::unique_ptr<LayoutState> state;
};

Compiler::LLVM::LayoutQueryScope::LayoutQueryScope(const Compiler::CompilerOptions &options)
    : _impl(std::make_unique<Impl>())
{
    Compiler::Subtarget sub;
    std::string error;
    const std::string triple = options.codegen.effective_triple();

    if (!Compiler::resolve_subtarget(
            triple, options.target_cpu, options.target_features, sub, error)) {
        throw Compiler::InternalCompilerException(error);
    }

    _impl->machine = make_target_machine(
        triple,
        sub.cpu,
        sub.features,
        codegen_opt_level(options.codegen_opt),
        options.codegen.uses_pic(),
        error
    );

    if (!_impl->machine) {
        throw Compiler::InternalCompilerException(error);
    }

    _impl->state = std::make_unique<LayoutState>(_impl->machine->createDataLayout());
    _impl->state->previous = t_current;
    t_current = _impl->state.get();
    _ast.emplace(&query_size, &query_align);
}

Compiler::LLVM::LayoutQueryScope::~LayoutQueryScope()
{
    if (_impl && _impl->state) {
        t_current = _impl->state->previous;
    }
}
