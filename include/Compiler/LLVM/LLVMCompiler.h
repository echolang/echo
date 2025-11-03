#ifndef LLVMCOMPILER_H
#define LLVMCOMPILER_H

#pragma once

#include "eco.h"
#include "AST/ASTBundle.h"
#include "AST/ASTVisitor.h"

#include "Compiler/CompilerException.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/AbortCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/Codegen/ExprCodegen.h"
#include "Compiler/LLVM/Codegen/StmtCodegen.h"
#include "Compiler/LLVM/Codegen/TypeDeclCodegen.h"
#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/DebugPrintCodegen.h"
#include "Compiler/LLVM/Codegen/Backend.h"

#include <filesystem>
#include <set>
#include <functional>
#include <string>
#include <vector>

// the compiler facade and the sole AST::Visitor. it owns the shared CodegenContext and the codegen
// subsystems, orchestrates the compile of a bundle, and forwards each visit to the subsystem that
// owns that node kind. the actual lowering logic lives in the Codegen/ subsystems, not here
class LLVMCompiler : public AST::Visitor
{
public:
    LLVMCompiler(Compiler::CompilerOptions options);
    ~LLVMCompiler();

    // names the module whose file-scope statements become the program's entry point. Must be set before
    // compile_bundle; defaults to ECO_MAIN_MODULE_NAME
    void set_entry_module(const std::string &module_name);

    // `cached_modules` names the modules whose compiled object is being reused, so no code is generated for
    // them at all - see TypeLowering::create_cmp_units for why that is the only place it has to be said
    void compile_bundle(const AST::Bundle &bundle, const std::set<std::string> &cached_modules = {});

    // every ODR-shared symbol defined in more than one unit must be defined *identically*, or the linker
    // keeps an arbitrary one and the program silently gets the wrong body. That is the obligation
    // AST::FunctionEmission::t_odr_shared takes on, and it is the one property of this design nothing
    // else would notice being broken: there is no diagnostic, no crash and no wrong answer at compile
    // time, only a program that behaves differently depending on which unit the linker preferred.
    //
    // so it is checked rather than assumed, in a debug compiler, right before the units are merged. It
    // costs a string render per duplicated definition and nothing at all when there are none, which is
    // every single-module program
    void verify_odr_consistency();

    // emits a body into every unit that owes one for an ODR-shared definition, until no unit owes any.
    //
    // the loop is what makes it total: emitting a body runs the same lazy declaration paths a source body
    // does, so `array<Padded>::reserve` naming `mem::realloc<Padded>` appends to the queue being drained.
    // An up-front closure could not compute this set - a cloned instance's call nodes live in the
    // template's module, not the referencing one, and an interface vtable names an implementation after
    // build_function_maps has already finished
    void drain_pending_definitions();

    void visitScope(AST::ScopeNode &node);
    void visitType(AST::TypeNode &node);
    void visitTypeCast(AST::TypeCastNode &node);
    void visitVarDecl(AST::VarDeclNode &node);
    void visitVarRef(AST::VarRefNode &node);
    void visitLiteralFloatExpr(AST::LiteralFloatExprNode &node);
    void visitLiteralIntExpr(AST::LiteralIntExprNode &node);
    void visitLiteralBoolExpr(AST::LiteralBoolExprNode &node);
    void visitLiteralStringExpr(AST::LiteralStringExprNode &node);
    void visitFunctionCallExpr(AST::FunctionCallExprNode &node);
    void visit_addr_of_expr(AST::AddrOfExprNode &node);
    void visit_deref_expr(AST::DerefExprNode &node);
    void visit_pointer_value(AST::PointerValueNode &node);
    void visit_move_expr(AST::MoveExprNode &node);
    void visit_class_alloc_expr(AST::ClassAllocExprNode &node);
    void visit_retain_expr(AST::RetainExprNode &node);
    void visit_strong_expr(AST::StrongExprNode &node);
    void visit_guard(AST::GuardNode &node);
    void visit_null_coalesce(AST::NullCoalesceExprNode &node);
    void visit_optional_chain(AST::OptionalChainExprNode &node);
    void visit_chain_base(AST::ChainBaseNode &node);
    void visit_closure_expr(AST::ClosureExprNode &node);
    void visit_indirect_call_expr(AST::IndirectCallExprNode &node);
    void visit_instanceof_expr(AST::InstanceOfExprNode &node);
    void visit_temporary_bind(AST::TemporaryBindExprNode &node);
    void visit_release(AST::ReleaseNode &node);
    void visit_index_expr(AST::IndexExprNode &node);
    void visit_array_literal_expr(AST::ArrayLiteralExprNode &node);
    void visitBinaryExpr(AST::BinaryExprNode &node);
    void visitUnaryExpr(AST::UnaryExprNode &node);
    void visitNull(AST::NullNode &node);
    void visitOperator(AST::OperatorNode &node);
    void visitFunctionDecl(AST::FunctionDeclNode &node);
    void visitReturn(AST::ReturnNode &node);
    void visitIfStatement(AST::IfStatementNode &node);
    void visitWhileStatement(AST::WhileStatementNode &node);
    void visit_loop_control(AST::LoopControlNode &node);
    void visit_foreach(AST::ForeachNode &node);
    void visit_assign(AST::AssignNode &node);
    void visitNamespaceDecl(AST::NamespaceDeclNode &node);
    void visitNamespace(AST::NamespaceNode &node);
    void visitAttribute(AST::AttributeNode &node);
    void visit_type_decl(AST::TypeDeclNode &node);
    void visitMemberAccess(AST::MemberAccessNode &node);
    void visitVar(AST::VarNode &node);

    // folds every compilation unit into the main module. Must run before any output that can only look at
    // one module - optimize(), printIR(), run_code() - and must *not* run when per-unit objects are wanted,
    // because it consumes them
    void link_into_main();

    void optimize();

    // drops everything the entry point cannot reach - `run` only, see Backend::prune_to_entry
    void prune_to_entry();

    void printIR(bool toFile);
    void run_code();

    // false when no binary was produced, see Backend::make_exec
    bool make_exec(std::string executable_name);

    // one object per unit that still has a module, into `object_for(unit name)`. The objects are appended to
    // `out_objects` in unit order, so the link command is deterministic
    bool emit_objects(
        const std::function<std::filesystem::path(const std::string &)> &object_for,
        std::vector<std::filesystem::path> &out_objects);

    // links what emit_objects produced, plus anything a cache supplied
    bool link_executable(
        const std::string &executable_name, const std::vector<std::filesystem::path> &objects);

private:
    Compiler::LLVM::CodegenContext _ctx;

    Compiler::LLVM::TypeLowering _types;
    Compiler::LLVM::LValueCodegen _lvalues;
    Compiler::LLVM::ExprCodegen _expr;
    Compiler::LLVM::StmtCodegen _stmt;
    Compiler::LLVM::TypeDeclCodegen _struct;
    Compiler::LLVM::ClassCodegen _classes;
    Compiler::LLVM::AbortCodegen _abort;
    Compiler::LLVM::DebugPrintCodegen _debug_print;
    Compiler::LLVM::Backend _backend;
};

#endif
