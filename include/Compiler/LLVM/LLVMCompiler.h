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
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

#include <map>
#include <memory>
#include <string>
#include <stack>

class LLVMCompiler : public AST::Visitor
{
    std::unique_ptr<llvm::LLVMContext> llvm_context;
    std::unique_ptr<llvm::IRBuilder<>> llvm_builder;
    std::unique_ptr<llvm::Module> llvm_module;
    
    std::stack<llvm::Value *> value_stack;

public:
    LLVMCompiler();
    ~LLVMCompiler();

    void compile_bundle(const AST::Bundle &bundle);

    virtual void visitScope(AST::ScopeNode &node);
    virtual void visitType(AST::TypeNode &node);
    virtual void visitVarDecl(AST::VarDeclNode &node);
    virtual void visitLiteralFloatExpr(AST::LiteralFloatExprNode &node);
    virtual void visitLiteralIntExpr(AST::LiteralIntExprNode &node);
    virtual void visitLiteralBoolExpr(AST::LiteralBoolExprNode &node);

    void printIR(bool toFile);
    void run_code();
    void make_exec(std::string executable_name);

private:

};

#endif