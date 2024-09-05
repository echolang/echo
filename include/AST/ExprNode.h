#ifndef EXPRESSIONNODE_H
#define EXPRESSIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Token.h"

#include "OperatorNode.h"

#include <optional>

namespace AST
{
    class FunctionDeclNode;
    class VarRefNode;
    class TypeNode;

    class ExprNode : public Node
    {
    public:
        // returns the type this expression will return
        virtual ValueType result_type() const {
            return ValueType::void_type();
        }

        bool is_implcit = false;

        ExprNode() {};
        ExprNode(bool implicit) : is_implcit(implicit) {};
        virtual ~ExprNode() {};

    private:
    };

    class VoidExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_void);

        VoidExprNode() {};
        ~VoidExprNode() {};

        const std::string node_description() override {
            return "void";
        }

        ValueType result_type() const override {
            return ValueType::void_type();
        }

        // void goes into the void
        void accept(Visitor &visitor) override {}

        Node *clone(CloneContext &cc) const override;
    };

    class FunctionCallExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_call);

        TokenReference token_function_name;
        std::vector<ExprNode*> arguments;

        // explicit type arguments written at the call site, e.g. foo<int>(...). Empty when the
        // call relies on inference. The monomorphizer prefers these over inferred type args.
        std::vector<TypeNode*> explicit_type_args;

        FunctionDeclNode *decl = nullptr;

        FunctionCallExprNode(TokenReference token_function_name, std::vector<ExprNode*> arguments) :
            token_function_name(token_function_name), arguments(arguments)
        {};

        ~FunctionCallExprNode() {}

        ValueType result_type() const override;

        const std::string decorated_func_name() const;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visitFunctionCallExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    class BinaryExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_binary);

        OperatorNode *op_node;
        ExprNode *lhs = nullptr;
        ExprNode *rhs = nullptr;

        BinaryExprNode(OperatorNode *op_node, ExprNode *lhs, ExprNode *rhs) :
            op_node(op_node), lhs(lhs), rhs(rhs)
        {};
        ~BinaryExprNode() {}

        ValueType result_type() const override;

        const std::string lhs_node_description() {
            return lhs ? lhs->node_description() : "[undefined]";
        }

        const std::string rhs_node_description() {
            return rhs ? rhs->node_description() : "[undefined]";
        }

        const std::string node_description() override {
            return "binexp<" + result_type().get_type_desciption() + ">(" + lhs_node_description() + " " + op_node->token_literal.value() + " " + rhs_node_description() + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visitBinaryExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    class UnaryExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_unary);

        TokenReference token_operator;

        ExprNode *expr;

        UnaryExprNode(TokenReference token_operator, ExprNode *expr) :
            token_operator(token_operator), expr(expr)
        {};

        ~UnaryExprNode() {}

        // negation preserves the operand type
        ValueType result_type() const override {
            return expr->result_type();
        }

        const std::string node_description() override {
            return "unexp(" + token_operator.value() + expr->node_description() + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visitUnaryExpr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `&E` - the address of the storage E denotes.
    //
    // deliberately takes the *storage* type, with no transparency peeling, so `&$buf` on a
    // `ptr<uint8>` is a `ptr<ptr<uint8>>`: the address of $buf's own slot, not the address
    // $buf holds (book/concept/pointers_and_refs_v2.md, "Pointers to pointers")
    class AddrOfExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_addrof);

        // any place expression - see AST::is_place_expression. a variable, a member access,
        // and later an index; not a temporary, which has no address to take
        ExprNode *operand;

        AddrOfExprNode(ExprNode *operand) :
            operand(operand)
        {
            assert(operand != nullptr && "AddrOfExprNode requires an operand");
        };

        ~AddrOfExprNode() {}

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_addr_of_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `E:$` - the pointer itself, rather than the thing it points at.
    //
    // this emits no code. `E:$` is not an operation on a value: it is *exactly what E already
    // is* before the transparency auto-deref, so the node's only job is to mark the position
    // so the adjustment pass does not insert that deref. the pass then erases it.
    //
    // it exists as a node, rather than a flag on ExprNode, so `$x:$` on a non-pointer has a
    // located object to report against after monomorphization - and because a flag on the
    // expression base class would be the same mistake the pointer bit-flag was.
    // reaching codegen is a compiler bug, and the visitor there says so
    class PointerValueNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_peel);

        ExprNode *operand;

        // the `:$` token, so the "nothing to peel" diagnostic can point at it
        TokenReference token_peel;

        PointerValueNode(ExprNode *operand, TokenReference token_peel) :
            operand(operand), token_peel(token_peel)
        {
            assert(operand != nullptr && "PointerValueNode requires an operand");
        };

        ~PointerValueNode() {}

        // the pointer, unpeeled - the operand's own type
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_pointer_value(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `E[n]` - the element n positions along from the address E holds.
    //
    // a place, so it reads and writes alike, and `$p:$[0]` is the same storage as `$p`. the
    // offset is scaled by the size of the pointee, never by bytes: `$it:$ + 1` on a ptr<int32>
    // advances four bytes (book/concept/pointers_and_refs_v2.md, "Pointer arithmetic")
    class IndexExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_index);

        // evaluated as a pointer-typed value: the address to offset from
        ExprNode *base;
        ExprNode *index;

        TokenReference token_bracket;

        IndexExprNode(ExprNode *base, ExprNode *index, TokenReference token_bracket) :
            base(base), index(index), token_bracket(token_bracket)
        {
            assert(base != nullptr && "IndexExprNode requires a base");
            assert(index != nullptr && "IndexExprNode requires an index");
        };

        ~IndexExprNode() {}

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_index_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // one auto-deref: reads the operand's pointer and yields the value at it.
    //
    // never written by the user. the pointer adjustment pass inserts one wherever a pointer is
    // read in value position, which is what lets every other node's result_type() be honest -
    // before, a pointer variable's read claimed `ptr<int32>` while codegen had already produced
    // an int32, and nothing downstream reconciled the two
    class DerefExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_deref);

        ExprNode *operand;

        DerefExprNode(ExprNode *operand) :
            operand(operand)
        {
            assert(operand != nullptr && "DerefExprNode requires an operand");
        };

        ~DerefExprNode() {}

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_deref_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

};

#endif