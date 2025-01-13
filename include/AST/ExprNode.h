#ifndef EXPRESSIONNODE_H
#define EXPRESSIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "Token.h"

#include "OperatorNode.h"

#include <optional>

namespace AST
{
    class FunctionDeclNode;
    class Namespace;
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

    // how far a call has been taken towards being resolved
    //
    // resolution is *attempted* where the call is written, because the call's type is needed there -
    // `$x = f(1);` takes the variable's type from it. but an argument's type is not necessarily
    // final at that moment: a local initialized from a generic constructor carries the template's
    // type until the monomorphizer's fixpoint answers it. a decision made against a type that says
    // nothing is a wrong decision rather than a missing one, so it is deferred instead, and the
    // fixpoint that answers those types is what finishes the call
    enum class CallSettlement
    {
        // no declaration yet: several candidates remain and the arguments that would separate them
        // have no type. the candidate set is re-derived when the question is asked again, never
        // stored - a stored set goes stale the moment the tree is cloned for an instantiation
        t_unresolved,

        // a declaration is chosen, but the arguments have not been fitted to its parameters
        t_uncoerced,

        // declaration chosen, arguments fitted. nothing further is owed - and nothing may touch the
        // arguments again, because a second coercion would wrap what the first one wrapped
        t_settled,
    };

    class FunctionCallExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_call);

        TokenReference token_function_name;
        std::vector<ExprNode*> arguments;

        // explicit type arguments written at the call site, e.g. foo<int>(...). Empty when the
        // call relies on inference. The monomorphizer prefers these over inferred type args
        std::vector<TypeNode*> explicit_type_args;

        FunctionDeclNode *decl = nullptr;

        // maintained only by AST::CallResolver, which is the one thing that sets `decl` and the one
        // thing that coerces the arguments - so the two can never disagree about what is done
        //
        // CloneContext::shallow copy-constructs, and inheriting this is exactly right in all three
        // states: a settled call's coercion nodes are cloned with it and must not be redone, while
        // an unresolved or uncoerced one is retried against the substituted types
        CallSettlement settlement = CallSettlement::t_unresolved;

        // where a free call looked its name up, so the lookup can be repeated in a later round.
        // null for a member call, whose candidates come from its receiver's type instead - which is
        // argument 0, so a member call needs nothing stored at all
        const Namespace *lookup_namespace = nullptr;

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

    // `&E` - the address of the storage E denotes
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

    // `E:$` - the pointer itself, rather than the thing it points at
    //
    // this emits no code. `E:$` is not an operation on a value: it is *exactly what E already
    // is* before the transparency auto-deref, so the node's only job is to mark the position
    // so the adjustment pass does not insert that deref. the pass then erases it
    //
    // it exists as a node, rather than a flag on ExprNode, so `$x:$` on a non-pointer has a
    // located object to report against after monomorphization - and because a flag on the
    // expression base class would be the same mistake the pointer bit-flag was
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

    // `mv E` - take the value out of E, leaving E unset
    //
    // like `:$` this emits no code, and for the same reason: a move *is* the copy that would
    // otherwise have been there, minus the leaving-behind. so the node's only job is to mark the
    // position, and AST::OwnershipPass then reads it twice - to skip the copy diagnostic, and to add
    // E's root variable to the moved set - before erasing it. nothing about a move survives into the
    // IR except the absence of the copy (book/concept/ownership_and_moving.md, "`mv` should erase
    // itself")
    //
    // deliberately *not* a place expression: the whole point is that E has stopped holding the
    // value, so `&(mv $a)` and `mv $a = ...` are nonsense. reaching codegen is a compiler bug, and
    // the visitor there says so
    class MoveExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_move);

        ExprNode *operand;

        // the `mv` token, so "you cannot move that" can point at the keyword
        TokenReference token_move;

        MoveExprNode(ExprNode *operand, TokenReference token_move) :
            operand(operand), token_move(token_move)
        {
            assert(operand != nullptr && "MoveExprNode requires an operand");
        };

        ~MoveExprNode() {}

        // the moved value's type - the operand's own. a move changes who owns it, not what it is
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_move_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // a fresh, zeroed heap block for a class, with its strong count seated at 1 and its typeinfo
    // written - the value a class constructor's `$this` is initialized with
    //
    // there is no syntax for this and there is not meant to be: `Foo(...)` builds either storage class,
    // and which one it is, is the declaration's business rather than the call site's. so the node is
    // synthesized by the type declaration parser, in exactly the place a struct constructor leaves
    // `$this` uninitialized, and everything else about a constructor - the property writes, the
    // implicit `return $this` - is the struct path unchanged.
    //
    // carries the class type rather than a ComplexType so result_type() can answer without help, which
    // is what lets the ownership pass see the +1 without knowing this node exists
    class ClassAllocExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_class_alloc);

        // the class being allocated. a template's `$this` carries the self-application `Foo<T>`, which
        // the monomorphizer substitutes like any other type on the node
        ValueType class_type;

        // the type name token, so a failure to lower the layout has somewhere to point
        TokenReference token_type;

        ClassAllocExprNode(ValueType class_type, TokenReference token_type) :
            class_type(std::move(class_type)), token_type(std::move(token_type))
        {
            assert(class_type.is_class() && "ClassAllocExprNode requires a class type");
        };

        ~ClassAllocExprNode() {}

        // the handle. a fresh block is +1 and belongs to whoever the value arrives at
        ValueType result_type() const override {
            return class_type;
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_class_alloc_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // one more strong reference to what E names, yielding E's own value
    //
    // wrapped around a class-typed *place* read wherever the value arrives somewhere that will owe a
    // release for it - a declaration, an assignment, a by-value argument. a class-typed value that is
    // not a place needs none: a constructor call or a function result is already one reference nobody
    // else holds, which is the same "a place is copied, a non-place is moved" rule the ownership pass
    // applies to structs
    //
    // in the tree rather than folded into codegen for the reason every implicit thing in this compiler
    // is: --print-resolved-ast shows exactly where the counting happens, which is the only practical
    // way to check a retain/release balance. AST::OwnershipPass decides, ClassCodegen emits
    class RetainExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_retain);

        ExprNode *operand;

        RetainExprNode(ExprNode *operand) : operand(operand)
        {
            assert(operand != nullptr && "RetainExprNode requires an operand");
        };

        ~RetainExprNode() {}

        // the operand's own type. a retain changes a count, not a value
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_retain_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `E instanceof T` - is the object E names an instance of exactly T?
    //
    // total over a class operand and constant-false against a struct, because a class block carries a
    // runtime type word and a struct carries nothing at all (CONCEPT.md, "structs do not have any
    // runtime meta data"). that asymmetry is the whole feature: the question is only answerable for a
    // value that brought its own answer along, so a *struct* operand is a compile error rather than a
    // `false`
    //
    // there is no inheritance, so "exactly T" is the only reading there is - the lowering is one
    // pointer comparison against the class's identity global
    class InstanceOfExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_instanceof);

        ExprNode *operand;

        // the type on the right. a named type either way - the diagnostic for a struct operand is the
        // *left* side's business, and a struct on the right is a legitimate question with the answer
        // `false`
        ValueType queried_type;

        // the `instanceof` keyword, so a bad operand reports at the operator
        TokenReference token_instanceof;

        InstanceOfExprNode(ExprNode *operand, ValueType queried_type, TokenReference token_instanceof) :
            operand(operand), queried_type(std::move(queried_type)), token_instanceof(std::move(token_instanceof))
        {
            assert(operand != nullptr && "InstanceOfExprNode requires an operand");
        };

        ~InstanceOfExprNode() {}

        ValueType result_type() const override {
            return ValueType(ValueTypePrimitive::t_bool);
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_instanceof_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `E[n]` - the element n positions along from the address E holds
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

    // one auto-deref: reads the operand's pointer and yields the value at it
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