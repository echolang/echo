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

    virtual void visitScope(AST::ScopeNode &node);
    virtual void visitType(AST::TypeNode &node);
    virtual void visitVarDecl(AST::VarDeclNode &node);
    virtual void visitVarRef(AST::VarRefNode &node);
    virtual void visitLiteralFloatExpr(AST::LiteralFloatExprNode &node);
    virtual void visitLiteralIntExpr(AST::LiteralIntExprNode &node);
    virtual void visitLiteralBoolExpr(AST::LiteralBoolExprNode &node);
    virtual void visitFunctionCallExpr(AST::FunctionCallExprNode &node);
    virtual void visitVarRefExpr(AST::VarRefExprNode &node);

    llvm::Type *get_llvm_type(AST::ValueTypePrimitive type);

    void optimize();
    void printIR(bool toFile);
    void run_code();
    void make_exec(std::string executable_name);

private:

};

#endif