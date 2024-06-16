#ifndef LLVMCOMPILER_H
#define LLVMCOMPILER_H

#pragma once

#include "AST/ASTBundle.h"
#include "AST/ASTVisitor.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <map>
#include <memory>
#include <string>
#include <stack>
#include <unordered_map>

namespace AST {
    class VarDeclNode;
};

class LLVMCompiler : public AST::Visitor
{
    std::unique_ptr<llvm::LLVMContext> llvm_context;
    std::unique_ptr<llvm::IRBuilder<>> llvm_builder;
    std::unique_ptr<llvm::Module> llvm_module;
    
    std::stack<llvm::Value *> value_stack;
    std::unordered_map<AST::VarDeclNode *, llvm::AllocaInst *> var_map;

public:
    LLVMCompiler();
    ~LLVMCompiler();

    void compile_bundle(const AST::Bundle &bundle);

    void visitScope(AST::ScopeNode &node);
    void visitType(AST::TypeNode &node);
    void visitVarDecl(AST::VarDeclNode &node);
    void visitVarRef(AST::VarRefNode &node);
    void visitLiteralFloatExpr(AST::LiteralFloatExprNode &node);
    void visitLiteralIntExpr(AST::LiteralIntExprNode &node);
    void visitLiteralBoolExpr(AST::LiteralBoolExprNode &node);
    void visitFunctionCallExpr(AST::FunctionCallExprNode &node);
    void visitVarRefExpr(AST::VarRefExprNode &node);
    void visitBinaryExpr(AST::BinaryExprNode &node);
    void visitUnaryExpr(AST::UnaryExprNode &node);
    void visitNull(AST::NullNode &node);
    void visitOperator(AST::OperatorNode &node);

    llvm::Type *get_llvm_type(AST::ValueTypePrimitive type);

    void optimize();
    void printIR(bool toFile);
    void run_code();
    void make_exec(std::string executable_name);

private:

};

#endif