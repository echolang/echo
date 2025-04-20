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

#include <string>

// the compiler facade and the sole AST::Visitor. it owns the shared CodegenContext and the codegen
// subsystems, orchestrates the compile of a bundle, and forwards each visit to the subsystem that
// owns that node kind. the actual lowering logic lives in the Codegen/ subsystems, not here
class LLVMCompiler : public AST::Visitor
{
public:
    LLVMCompiler(Compiler::CompilerOptions options);
    ~LLVMCompiler();

    void compile_bundle(const AST::Bundle &bundle);

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
    void visit_assign(AST::AssignNode &node);
    void visitNamespaceDecl(AST::NamespaceDeclNode &node);
    void visitNamespace(AST::NamespaceNode &node);
    void visitAttribute(AST::AttributeNode &node);
    void visit_type_decl(AST::TypeDeclNode &node);
    void visitMemberAccess(AST::MemberAccessNode &node);
    void visitVar(AST::VarNode &node);

    void optimize();
    void printIR(bool toFile);
    void run_code();

    // false when no binary was produced, see Backend::make_exec
    bool make_exec(std::string executable_name);

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
