#ifndef CODEGENCONTEXT_H
#define CODEGENCONTEXT_H

#pragma once

#include "eco.h"
#include "Compiler/CompilerException.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "AST/ASTCoreTypes.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <cassert>
#include <memory>
#include <optional>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

namespace AST
{
    class File;
    class Visitor;
    class VarDeclNode;
    class FunctionDeclNode;
};

namespace Compiler::LLVM
{
    class TypeLowering;
    class LValueCodegen;
    class ClassCodegen;
    class AbortCodegen;

    // shared mutable state threaded through every codegen subsystem. owns the llvm context and
    // builder, the per-module compilation units, and the transient value/variable bookkeeping the
    // visitor recursion relies on. the subsystems (TypeLowering, ExprCodegen, StmtCodegen,
    // TypeDeclCodegen, Backend) all hold a reference to one of these and talk to the shared state
    // exclusively through it
    struct CodegenContext
    {
        std::unique_ptr<llvm::LLVMContext> llvm_context;
        std::unique_ptr<llvm::IRBuilder<>> builder;

        // what the invocation asked for. read through its predicates rather than compared, so that
        // every check the compiler can skip skips together
        CompilerOptions options;

        // the stdlib types the compiler names, bound by `#[core: "..."]` during parsing. published here
        // by compile_bundle rather than reached for, so codegen never holds the whole collector - the
        // only thing it needs from it is which declared type is `string`
        const AST::CoreTypes *core_types_ptr = nullptr;

        const AST::CoreTypes &core_types() const {
            assert(core_types_ptr && "core types not published - compile_bundle must set them");
            return *core_types_ptr;
        }

        // where the fields of the bound `string` and `string::view` sit, resolved **once** beside the
        // binding above rather than per literal: it is a fact about the program, not about the node
        // being lowered, and the diagnostic for a malformed stdlib string should fire once whether or
        // not some literal happens to reach codegen. nullopt when no stdlib declared one at all
        std::optional<AST::CoreStringLayout> string_layout;

        const AST::CoreStringLayout &core_string_layout() const {
            assert(string_layout.has_value() && "string layout not resolved - compile_bundle must resolve it");
            return string_layout.value();
        }

        // the registry an interface **widening** needs, published here by compile_bundle for the reason
        // core_types_ptr above is - so codegen still never holds the whole collector.
        //
        // why codegen needs it at all: an erased value carries its vtable, and the vtable is resolved
        // where the concrete class is still known, which is the widening site. filling it means asking
        // AST::interface_implementations which declaration answers each requirement, and matching a
        // *generic* interface's requirement (`Comparable<Money>`'s `T`) against its implementor
        // re-substitutes that type. every such lookup is a cache hit by now - the type checker resolved
        // the same conformance before codegen ran - so this interns nothing new
        AST::TypeRegistry *type_registry_ptr = nullptr;

        AST::TypeRegistry &type_registry() const {
            assert(type_registry_ptr && "type registry not published - compile_bundle must set it");
            return *type_registry_ptr;
        }

        // the host target's data layout and triple, published by Backend::init_target before any
        // module is created so that every module carries them from the start. this is what makes
        // a compile-time `size_of<T>()` answer the same number the running program will see -
        // asking a layout-less module gives LLVM's defaults, which match no real target
        std::optional<llvm::DataLayout> data_layout;
        std::string target_triple;

        const llvm::DataLayout &layout() const {
            assert(data_layout.has_value() && "target not initialized - Backend::init_target must run before codegen");
            return data_layout.value();
        }

        std::vector<std::unique_ptr<CmpUnit>> cmp_units;
        std::unordered_map<std::string, CmpUnit *> cmp_unit_map;

        CmpUnit *current_cmp_unit = nullptr;
        AST::File *current_file = nullptr;

        // the function declaration currently being generated, set/restored around each function
        // body so codegen errors can name their enclosing function. null at global scope
        AST::FunctionDeclNode *current_function = nullptr;

        std::stack<llvm::Value *> value_stack;
        std::unordered_map<AST::VarDeclNode *, llvm::AllocaInst *> var_map;

        // the owning LLVMCompiler, so subsystems can recurse into child nodes through the single
        // AST::Visitor that the node accept() dispatch requires.
        AST::Visitor *visitor = nullptr;

        // the type-lowering subsystem, reachable from any subsystem that needs to map an
        // AST::ValueType to an llvm::Type.
        TypeLowering *types = nullptr;

        // the lvalue subsystem: the single place that turns an expression into an address
        // every read, write and address-of goes through it, so they cannot drift apart
        LValueCodegen *lvalues = nullptr;

        // the class subsystem: allocation and the two reference-count operations. reachable from the
        // statement and expression subsystems, which is where the tree says a retain or a release goes
        ClassCodegen *classes = nullptr;

        // the abort subsystem: the one owner of how a program stops. every stop site - `die`, a
        // failed `assert`, the null narrowing check - goes through it, so they share one runtime,
        // one message shape and one release-mode gate
        AbortCodegen *abort = nullptr;

        llvm::Module *current_module() {
            return current_cmp_unit->llvm_module.get();
        }

        // declares a libc function into the current module, or hands back the one already there
        //
        // the RC runtime, the abort runtime and `echo` are all *emitted* rather than linked, so
        // each needs a handful of C symbols with no stdlib declaration behind them. spelled out per
        // symbol they had already drifted - `printf` still uses the legacy typed pointer, and only
        // `exit` remembered its attributes - so this is the one spelling
        llvm::FunctionCallee libc_callee(
            const char *name,
            llvm::Type *return_type,
            llvm::ArrayRef<llvm::Type *> parameter_types,
            bool variadic = false)
        {
            return current_module()->getOrInsertFunction(
                name, llvm::FunctionType::get(return_type, parameter_types, variadic));
        }

        // the opaque `ptr`, spelled once - it appears in almost every emitted runtime signature
        llvm::Type *opaque_ptr_type() const {
            return llvm::PointerType::get(*llvm_context, 0);
        }

        // **has the block being emitted into already ended?** one question with many askers, and
        // the answer to all of them is "then emit nothing more here": gen_scope stops walking a
        // scope, gen_function_decl declines to synthesize a second terminator, the if/while arms
        // decline to branch, and compile_bundle's main epilogue declines to return
        //
        // named because a second terminator in one block fails the verifier, so every emitter that
        // can follow a `return` or a `die` owes this check - and the next early exit (`break`) will
        // owe it too
        bool block_is_terminated() const {
            llvm::BasicBlock *block = builder->GetInsertBlock();
            return block != nullptr && block->getTerminator() != nullptr;
        }

        void push(llvm::Value *value) {
            value_stack.push(value);
        }

        llvm::Value *pop() {
            auto value = value_stack.top();
            value_stack.pop();
            return value;
        }

        // the main compilation unit, or nullptr if the bundle has no main module yet
        CmpUnit *main_cmp_unit();

        std::string llvm_err_str();

        // a human-readable description of the current codegen location, e.g. "in function 'foo'"
        // or "at global scope", suffixed onto codegen error messages
        std::string function_context() const;

        // the name of the file being emitted. the *file name*, not the path: the e2e runner passes
        // an absolute source directory, so a path would make every golden machine-specific
        //
        // beside function_context() rather than on a subsystem, because it answers the same kind of
        // question from the same state, and the one caller that needs both would otherwise reach
        // through two different owners for one sentence of output
        std::string current_file_name() const;

        Compiler::InternalCompilerException error(std::string message);
    };
};

#endif
