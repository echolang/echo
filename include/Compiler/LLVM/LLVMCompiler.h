#ifndef LLVMCOMPILER_H
#define LLVMCOMPILER_H

#pragma once

#include "eco.h"
#include "AST/ASTBundle.h"
#include "AST/ASTVisitor.h"

#include "Compiler/CompilerException.h"
#include "Compiler/LLVM/CompilationUnit.h"

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
    class File;
    class VarDeclNode;
    class VarMutNode;
};

class LLVMCompiler : public AST::Visitor
{
    std::string get_llvm_err_str();


    llvm::Module *curr_llvm_module() {
        return _current_cmp_unit->llvm_module.get();
    }


    llvm::Function *create_llvm_func_decl(const AST::FunctionDeclNode *node, Compiler::LLVM::CmpUnit &cmp_unit);
    
    void build_function_maps(const AST::Bundle &bundle);


public:
    LLVMCompiler();
    ~LLVMCompiler();

    Compiler::InternalCompilerException make_internal_compiler_error(std::string message);

    void compile_bundle(const AST::Bundle &bundle);

    void visitScope(AST::ScopeNode &node);
    void visitType(AST::TypeNode &node);
    void visitTypeCast(AST::TypeCastNode &node);
    void visitVarDecl(AST::VarDeclNode &node);
    void visitVarRef(AST::VarRefNode &node);
    void visitLiteralFloatExpr(AST::LiteralFloatExprNode &node);
    void visitLiteralIntExpr(AST::LiteralIntExprNode &node);
    void visitLiteralBoolExpr(AST::LiteralBoolExprNode &node);
    void visitFunctionCallExpr(AST::FunctionCallExprNode &node);
    void visitVarRefExpr(AST::VarRefExprNode &node);
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

    llvm::Type *get_llvm_type(AST::ValueTypePrimitive type);

    void optimize();
    void printIR(bool toFile);
    void run_code();
    void make_exec(std::string executable_name);

private:

    Compiler::LLVM::CmpUnit *get_main_cmpu();

    void create_cmp_units(const AST::Bundle &bundle);

    std::vector<std::unique_ptr<Compiler::LLVM::CmpUnit>> _cmp_units;
    std::unordered_map<std::string, Compiler::LLVM::CmpUnit *> _cmp_unit_map;

    Compiler::LLVM::CmpUnit *_current_cmp_unit = nullptr;
    AST::File *_current_file = nullptr;

    std::unique_ptr<llvm::LLVMContext> llvm_context;
    std::unique_ptr<llvm::IRBuilder<>> llvm_builder;
    
    std::stack<llvm::Value *> value_stack;
    std::unordered_map<AST::VarDeclNode *, llvm::AllocaInst *> var_map;

};

#endif