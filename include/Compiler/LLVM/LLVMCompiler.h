#ifndef LLVMCOMPILER_H
#define LLVMCOMPILER_H

#pragma once

#include "eco.h"
#include "AST/ASTBundle.h"
#include "AST/ASTVisitor.h"

#include "Compiler/CompilerException.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/ExprCodegen.h"
#include "Compiler/LLVM/Codegen/StmtCodegen.h"
#include "Compiler/LLVM/Codegen/StructCodegen.h"
#include "Compiler/LLVM/Codegen/Backend.h"

#include <string>

// the compiler facade and the sole AST::Visitor. it owns the shared CodegenContext and the codegen
// subsystems, orchestrates the compile of a bundle, and forwards each visit to the subsystem that
// owns that node kind. the actual lowering logic lives in the Codegen/ subsystems, not here.
class LLVMCompiler : public AST::Visitor
{
public:
    LLVMCompiler();
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
    void visitVarPtrExpr(AST::VarPtrExprNode &node);
    void visitBinaryExpr(AST::BinaryExprNode &node);
    void visitUnaryExpr(AST::UnaryExprNode &node);
    void visitNull(AST::NullNode &node);
    void visitOperator(AST::OperatorNode &node);
    void visitFunctionDecl(AST::FunctionDeclNode &node);
    void visitReturn(AST::ReturnNode &node);
    void visitIfStatement(AST::IfStatementNode &node);
    void visitWhileStatement(AST::WhileStatementNode &node);
    void visitVarMut(AST::VarMutNode &node);
    void visitNamespaceDecl(AST::NamespaceDeclNode &node);
    void visitNamespace(AST::NamespaceNode &node);
    void visitAttribute(AST::AttributeNode &node);
    void visitStructDecl(AST::StructDeclNode &node);
    void visitMemberAccess(AST::MemberAccessNode &node);
    void visitVar(AST::VarNode &node);
    void visitVarMember(AST::VarMemberNode &node);
    void visitMemberMut(AST::MemberMutNode &node);

    void optimize();
    void printIR(bool toFile);
    void run_code();
    void make_exec(std::string executable_name);

private:
    Compiler::LLVM::CodegenContext _ctx;

    Compiler::LLVM::TypeLowering _types;
    Compiler::LLVM::ExprCodegen _expr;
    Compiler::LLVM::StmtCodegen _stmt;
    Compiler::LLVM::StructCodegen _struct;
    Compiler::LLVM::Backend _backend;
};

#endif
