#ifndef EXPRCODEGEN_H
#define EXPRCODEGEN_H

#pragma once

namespace llvm
{
    class Value;
    class Function;
};

namespace AST
{
    class FunctionDeclNode;
    class TypeCastNode;
    class VarRefNode;
    class LiteralFloatExprNode;
    class LiteralIntExprNode;
    class LiteralBoolExprNode;
    class LiteralStringExprNode;
    class BinaryExprNode;
    class UnaryExprNode;
    class FunctionCallExprNode;
    class ClosureExprNode;
    class IndirectCallExprNode;
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
    // pointer references) to llvm values, leaving the produced value on the context value stack
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

        // `{ fn, env }` from a closure literal: the body's address, and the environment the capture pass
        // put there - a null pointer when nothing is captured
        void gen_closure_expr(AST::ClosureExprNode &node);

        // a call through a callable value. the `fn` slot is extracted and called with the `env` slot
        // prepended, which is why every callable target takes the environment as parameter 0
        void gen_indirect_call(AST::IndirectCallExprNode &node);

        // answers a `#[builtin: ...]` call directly, as a constant, instead of emitting a call
        // the type being asked about comes from the instance's instantiation_args, which the
        // monomorphizer stamped on when it resolved `size_of<int32>()` from `size_of<T>()`
        void gen_builtin_call(AST::FunctionCallExprNode &node);
        void gen_addr_of(AST::AddrOfExprNode &node);
        void gen_deref(AST::DerefExprNode &node);
        void gen_index(AST::IndexExprNode &node);
        void gen_null(AST::NullNode &node);
        void gen_operator(AST::OperatorNode &node);

    private:
        CodegenContext &_ctx;

        // the llvm::Function a declaration was emitted as, searching this unit first and then every
        // other one - a declaration is emitted into exactly one unit, and a call may cross that line.
        // null when nothing was emitted for it; the caller phrases the diagnostic
        llvm::Function *find_llvm_function(const AST::FunctionDeclNode *decl);

        // traps when `address` is null. emitted where a nullable pointer is narrowed to a
        // borrow, which is the one conversion that asserts rather than merely reinterprets
        // debug builds only - in release the narrowing is unchecked, as the doc says
        void gen_null_assert(llvm::Value *address);
    };
};

#endif
