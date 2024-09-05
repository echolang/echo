#ifndef EXPRCODEGEN_H
#define EXPRCODEGEN_H

#pragma once

namespace llvm
{
    class Value;
};

namespace AST
{
    class TypeCastNode;
    class VarRefNode;
    class LiteralFloatExprNode;
    class LiteralIntExprNode;
    class LiteralBoolExprNode;
    class LiteralStringExprNode;
    class BinaryExprNode;
    class UnaryExprNode;
    class FunctionCallExprNode;
    class AddrOfExprNode;
    class DerefExprNode;
    class IndexExprNode;
    class NullNode;
    class OperatorNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers expression nodes (literals, casts, arithmetic/logical operators, calls, variable and
    // pointer references) to llvm values, leaving the produced value on the context value stack.
    class ExprCodegen
    {
    public:
        ExprCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_type_cast(AST::TypeCastNode &node);
        void gen_var_ref(AST::VarRefNode &node);
        void gen_literal_float(AST::LiteralFloatExprNode &node);
        void gen_literal_int(AST::LiteralIntExprNode &node);
        void gen_literal_bool(AST::LiteralBoolExprNode &node);
        void gen_literal_string(AST::LiteralStringExprNode &node);
        void gen_binary_expr(AST::BinaryExprNode &node);
        void gen_unary_expr(AST::UnaryExprNode &node);
        void gen_function_call(AST::FunctionCallExprNode &node);
        void gen_addr_of(AST::AddrOfExprNode &node);
        void gen_deref(AST::DerefExprNode &node);
        void gen_index(AST::IndexExprNode &node);
        void gen_null(AST::NullNode &node);
        void gen_operator(AST::OperatorNode &node);

    private:
        CodegenContext &_ctx;

        // traps when `address` is null. emitted where a nullable pointer is narrowed to a
        // borrow, which is the one conversion that asserts rather than merely reinterprets.
        // debug builds only - in release the narrowing is unchecked, as the doc says
        void gen_null_assert(llvm::Value *address);
    };
};

#endif
