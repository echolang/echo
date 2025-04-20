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

        // reported, and terminal. an ambiguous or unmatchable call is decided on types that are
        // already known, so no later round can change the answer - without this state the fixpoint
        // re-derives the whole match, and the diagnostic with it, once per round for every failed
        // call, and non-duplication rests on Collector::collect_issue de-duplicating a message it
        // should never have been handed twice
        t_failed,
    };

    // has the call been taken as far as it will go, whether that ended in a declaration or in a
    // diagnostic? the two loops in the fixpoint skip these, and `settle` answers from cache
    inline bool call_is_terminal(CallSettlement settlement)
    {
        return settlement == CallSettlement::t_settled || settlement == CallSettlement::t_failed;
    }

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

        // maintained by AST::CallResolver, which is the one thing that coerces the arguments, and by
        // AST::OwnershipPass for the calls it synthesizes with their callee already named - which it
        // publishes as t_uncoerced, so the state and `decl` cannot disagree about what is done
        //
        // CloneContext::shallow copy-constructs, and inheriting this is right in all four states: a
        // settled call's coercion nodes are cloned with it and must not be redone, an unresolved or
        // uncoerced one is retried against the substituted types, and a failed one stays failed
        // because the arms that fail are the two that no substitution changes - a tie decided on
        // known types, and an argument argument_fit answered t_none rather than t_undetermined for
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

    // **is this the decl-less print builtin?** `echo` borrows the call node's shape without being a
    // call: it names no declaration anywhere, and ExprCodegen lowers it from its own token into a
    // printf rather than through the function table - the `builtin` kind's "no symbol at all", spelled
    // as a statement
    //
    // one predicate because three passes have to agree about it - the parser, which settles the node
    // on the spot since there is nothing to look up; the type checker, which owns the "printf has a
    // conversion for this argument" diagnostic; and codegen, which lowers it. they used to compare the
    // *name* against a literal, once each, so a second construct of this kind meant finding all three
    //
    // the token type, not the name: `echo` is a whole-word lexer keyword (ECHO_LEX_FNC_KEYWORD), so
    // nothing else can carry that token, and a declaration can never be spelled with one
    inline bool is_print_call(const FunctionCallExprNode &call)
    {
        return call.decl == nullptr && call.token_function_name.type() == Token::Type::t_echo;
    }

    // **is this expression a string literal, and what does it say?** the text if it is, nullopt if
    // it is not, one question because two subsystems ask it about the same argument and must agree:
    // AST::TypeChecker rejects a `die`/`assert` message that has no answer, and ExprCodegen folds
    // the answer into the abort text
    //
    // spelled separately they were a silent failure rather than a diagnostic - codegen's copy
    // returned "" for a shape the checker had let through, so the message simply lost its detail
    //
    // implemented in ExprNode.cpp because it looks through the implicit casts the resolver wraps an
    // argument in, which is the same reason is_written_null does
    std::optional<std::string> literal_string_value(const ExprNode *expr);

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
    //
    // **over a class, where a weak reference is what is being asked for, it means one instead.** a class
    // handle already *is* an address, so the address of a slot holding one is rarely what a program wants;
    // what it wants is a second reference that does not own. `weak<Foo> $w = &$obj;` says so, and that
    // see book/concept/ownership_and_moving.md, "Weak references and cycles"
    //
    // **the destination decides, not the operand's type.** that is the rule this chapter already runs on -
    // a destination decides how far a value is read (`as_value_for`), and a `Foo&` parameter is what turns
    // a place argument into an address. it also has to be the rule here rather than "a written `&` over a
    // class", which was tried: six generic accessors in stdlib/core take `&` of a `T` place and need the
    // slot address, so keying on the operand made `Array<int32>` compile and `Array<Counter>` not - a
    // generic body whose meaning depends on its instantiation, which is the one thing a generic must not be
    //
    // this node is also how every *compiler-inserted* borrow is spelled - a method receiver
    // (FuncCallParser), a borrow-parameter coercion (CallResolver), a drop's receiver (OwnershipPass) -
    // and those must keep meaning the slot's address, or `$this` would stop being `Foo&`. one bit
    // separates them, rather than a second node, because every pass that special-cases `n_expr_addrof`
    // - place_root_of, PointerAdjuster::adjust_call_arguments, OwnershipPass's place edge - would
    // otherwise need a duplicate arm that could drift
    class AddrOfExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_addrof);

        // any place expression - see AST::is_place_expression. a variable, a member access,
        // and later an index; not a temporary, which has no address to take
        ExprNode *operand;

        // **was a weak reference what this `&` was asked for?** set by the unary arm in ExprParser when the
        // destination is a `weak<T>`, and unconditionally by `weak($obj)`, which says so itself. never by
        // a pass inserting a borrow, and never by the `$x:$:$` collapse - both mean the slot's address
        //
        // a stored bit rather than something derivable, because the destination is knowable only where the
        // node is built. read through denotes_weak_reference() below, so "what does this `&` denote" still
        // has exactly one owner
        bool weak_wanted = false;

        AddrOfExprNode(ExprNode *operand, bool weak_wanted = false) :
            operand(operand),
            weak_wanted(weak_wanted)
        {
            assert(operand != nullptr && "AddrOfExprNode requires an operand");
        };

        // true when this `&` denotes a weak reference rather than a slot address. the one place the rule
        // lives, asked by result_type(), node_description() and codegen
        //
        // the operand still has to be something *counted*, and a type parameter counts as maybe: inside a
        // template `&$t` has no answer yet, and after substitution the clone's operand is the concrete
        // class. that is the standing "ask again later" rule, and it is why this is a method rather than
        // the bit itself. an operand that substitutes to something uncounted falls back to the slot
        // address and fails to fit its `weak<T>` destination, which is a located error at the right place
        // the rule over a type the caller already holds. result_type() needs the operand's type either
        // way, and deriving it twice walks the operand's subtree twice - a `->` chain re-walks per link
        bool denotes_weak_reference(const ValueType &operand_type) const {
            return weak_wanted && (operand_type.is_class() || operand_type.is_type_param());
        }

        bool denotes_weak_reference() const {
            return weak_wanted && denotes_weak_reference(operand->result_type());
        }

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
    // `function(int32 $a) : int32 { ... }` written where a value is expected.
    //
    // the body is an ordinary FunctionDeclNode hoisted to the file root - a closure is not a special kind
    // of function, only a function nobody can name - and this node is what turns it into a *value*: the
    // fat pointer `{ fn, env }` that a `function<R(P...)>` is
    class ClosureExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_closure);

        // the anonymous declaration this literal makes a value of. non-owning, like every tree edge:
        // the declaration hangs off the file root's scope, which is what codegen emits bodies from
        FunctionDeclNode *decl = nullptr;

        // the environment the captures live in, or null when nothing is captured - the shape a
        // non-capturing closure keeps, and the reason a callable is a fat pointer rather than a handle
        //
        // a *type* rather than an expression: the block is allocated and filled at this expression, and
        // there is no Echo-level constructor to call, because the environment is a type the compiler
        // declared and no source ever names
        ComplexType *environment_type = nullptr;

        // one place expression per captured variable, in property order - evaluated *here*, in the frame
        // the closure is created in, which is what makes capture by value what it is. the closure's body
        // reads them back off the environment parameter
        std::vector<ExprNode *> captured_values;

        TokenReference token;

        ClosureExprNode(FunctionDeclNode *decl, TokenReference token) : decl(decl), token(token) {};

        ~ClosureExprNode() {}

        // the callable type its declaration describes, environment parameter excluded
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_closure_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `$f(1, 2)` - a call through a *value* rather than to a declaration.
    //
    // a distinct node rather than a `callee` field on FunctionCallExprNode, because `decl` there is not
    // one thing: it is the callee, the return type, the parameter list argument coercion walks, the
    // function-table key and the name every diagnostic prints. an indirect call answers all five from its
    // callee's type instead, and has no overload set to resolve - so it is settled the moment it parses,
    // the same standing `echo` already has
    class IndirectCallExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_indirect_call);

        ExprNode *callee = nullptr;
        std::vector<ExprNode *> arguments;

        TokenReference token;

        IndirectCallExprNode(ExprNode *callee, std::vector<ExprNode *> arguments, TokenReference token) :
            callee(callee), arguments(std::move(arguments)), token(token) {};

        ~IndirectCallExprNode() {}

        // what the callee reads as: a `ptr<function<...>>` is read through first, which is the one
        // thing every reader of this node has to know. void when there is no callee - callers ask
        // is_callable() from here, and the type they get back is also what a diagnostic names
        ValueType callee_type() const;

        // the callee's signature's return type. void when the callee is not (yet) callable, so a
        // half-resolved tree answers rather than asserting - the same contract a call with no decl has
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_indirect_call_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // true when the expression is a call, whichever of the two kinds above it is. a caller asking this
    // is asking "may this expression stand alone as a statement" or "is a value being produced by
    // invoking something" - neither question cares which, and spelling out one of the two tags is how
    // `$h->op(41);` came to be rejected while `echo $h->op(41);` worked
    //
    // here rather than with the place predicates: the two nodes it enumerates are declared right above
    // it, so a third call node cannot be added without seeing it
    inline bool is_call_expression(const ExprNode &expr)
    {
        return expr.get_node_type() == NodeType::n_expr_call
            || expr.get_node_type() == NodeType::n_expr_indirect_call;
    }

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

    // `strong(E)` - the upgrade of a weak reference back to a usable one
    //
    // the inverse of a written `&` over a class, and the only way to read through a `weak<T>` at all. it
    // is a written operation rather than an implicit deref for two reasons, and both matter: it moves the
    // strong count, so it must be visible at the site that pays for it, and **it can fail** - the object
    // may already be gone - so its result is a `T?` the program has to acknowledge before using
    //
    // a keyword form rather than a `#[builtin:]` in stdlib/core/rc.eco, unlike `ref_count` beside it,
    // because `weak` has to be a keyword anyway to be a type constructor, and a language whose two halves
    // of one idea live in different places - one in the grammar, one in a library that `--no-stdlib`
    // removes - would be answering "where is weak from" twice
    class StrongExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_strong);

        // an expression of weak type. a place is not required: the upgrade reads the handle, and a weak
        // handed back by a call is as upgradable as one sitting in a variable
        ExprNode *operand;

        // where `strong` was written, for the diagnostic when the operand is not a weak
        TokenReference token;

        StrongExprNode(ExprNode *operand, TokenReference token) :
            operand(operand),
            token(token)
        {
            assert(operand != nullptr && "StrongExprNode requires an operand");
        };

        ~StrongExprNode() {}

        // `T?` for a `weak<T>` operand - nullable, which is the whole point. an operand that is not a
        // weak answers unknown rather than asserting: the type checker reports it with a location, and a
        // generic whose operand is still a bare parameter has to be able to be asked early
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_strong_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `A ?? B` - A when it is there, B otherwise
    //
    // one of the three forms that read through a `T?`, and the one for when there is a sensible stand-in.
    // it works on any nullable whatever the payload is - `lookup($k) ?? 0`, `$p ?? Point(0, 0)`,
    // `$maybeNode ?? $fallback` - and on a `weak<T>`, which AST::optional_operand_of upgrades first
    //
    // **B is evaluated only when A is absent.** so the right side may be as expensive as it likes, and
    // may have effects that must not happen on the common path
    class NullCoalesceExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_null_coalesce);

        // already a nullable by the time this node exists - a weak operand was rewritten at construction
        ExprNode *lhs;
        ExprNode *rhs;

        TokenReference token;

        NullCoalesceExprNode(ExprNode *lhs, ExprNode *rhs, TokenReference token) :
            lhs(lhs), rhs(rhs), token(token)
        {
            assert(lhs != nullptr && rhs != nullptr && "NullCoalesceExprNode requires both operands");
        };

        ~NullCoalesceExprNode() {}

        // the non-null of the left, which is the point of the whole form: `int32? ?? int32` is an `int32`,
        // and the result needs no further unwrapping
        //
        // unless the right side is *itself* nullable, in which case the answer still may be absent and the
        // type says so - `$a ?? $b` over two `int32?`s is an `int32?`, which chains
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_null_coalesce(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // stands for the **unwrapped base** inside the continuation of a `?->` chain
    //
    // `$a?->b->c` is one `OptionalChainExprNode` whose continuation is `<base>->b->c`, and this node is
    // that `<base>`: the value the chain already tested, with its `?` taken off. it is not an expression a
    // program can write, and it never appears outside a chain's continuation
    //
    // it exists so the continuation is an **ordinary member-access subtree** - the same nodes `->` always
    // builds, resolving members and calls through exactly the usual rules. the alternative was a chain node
    // that re-implemented member lookup for itself, which is the kind of second answer this compiler
    // reliably regrets
    class ChainBaseNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_chain_base);

        // the non-null type of the chain's base. stored rather than derived, because by the time this is
        // asked the base expression belongs to the enclosing chain node and this one has no edge to it
        ValueType type;

        TokenReference token;

        ChainBaseNode(ValueType type, TokenReference token) : type(type), token(token) {}

        ~ChainBaseNode() {}

        ValueType result_type() const override {
            return type;
        }

        const std::string node_description() override {
            return "chainbase<" + type.get_type_desciption() + ">";
        }

        void accept(Visitor &visitor) override {
            visitor.visit_chain_base(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `A?->b` - reach through A when it is there, and answer null when it is not
    //
    // the third of the forms, and the one for when absence simply means "nothing to do". it short-circuits:
    // if the base is absent the continuation does not run at all, so `$a?->save()` on an absent `$a` calls
    // nothing rather than calling something on null
    //
    // the continuation is an ordinary member-access or call subtree rooted at a ChainBaseNode, so `->`
    // resolves through the rules it always does. the parser wraps once per `?->`, which is what makes
    // `$a?->b?->c` nest and stop at the first absent link
    class OptionalChainExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_optional_chain);

        // the nullable being reached through - already upgraded if it was a weak
        ExprNode *base;

        // rooted at `chain_base`, which stands for `base` with its `?` removed
        ExprNode *continuation;

        ChainBaseNode *chain_base;

        TokenReference token;

        OptionalChainExprNode(
            ExprNode *base, ExprNode *continuation, ChainBaseNode *chain_base, TokenReference token) :
            base(base), continuation(continuation), chain_base(chain_base), token(token)
        {
            assert(base != nullptr && "OptionalChainExprNode requires a base");
        };

        ~OptionalChainExprNode() {}

        // the continuation's type made nullable - because the whole expression is absent whenever the base
        // was. `void` stays `void`: a call that answers nothing has nothing to be absent, and wrapping it
        // would invent a value for a statement to discard
        //
        // an already-nullable continuation is **not** wrapped twice. there is one `null` in the language,
        // so `$a?->maybeB()` is one `B?` rather than a nested absence nobody could spell
        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_optional_chain(*this);
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

    // `E[...]` - an element of whatever E names. **one node for both meanings of a bracket**, because
    // place-ness is structural: AST::is_place_expression answers on the tag, and a second "container
    // index" node would have to re-derive it (the mistake MemberAccessNode::result_type() made, B16)
    //
    // which meaning it has is decided from the base's type by AST::OperatorRewriter, inside the
    // monomorphizer's fixpoint - the parser cannot know, because the base may be typed from a call
    // that has not settled or from a type parameter that has not been substituted:
    //
    //  - **a pointer**, and then only when the base was written `:$`. the element n positions along
    //    from the address, offset scaled by the size of the pointee, never by bytes
    //    (book/concept/pointers_and_refs_v2.md, "Pointer arithmetic")
    //  - **a container**, and then `element_call` holds the `operator []` the base's type declares.
    //    the operator returns a borrow, so what the call yields *is* the element's address, which is
    //    exactly what gen_lvalue's other arm produces itself
    class IndexExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_index);

        // the pointer-typed value to offset from. **null once `element_call` is set**: the operands
        // move into the call rather than being shared with it, because an edge with two parents is
        // one PointerAdjuster rewrites twice
        ExprNode *base;

        // `$a[$i]`, `$m[$row, $col]`, and **empty** for the append form `$a[]`. a list because arity
        // is what tells one `operator []` overload from another, and match_function compares it first
        std::vector<ExprNode *> indices;

        // was the base written with `:$`? recorded at parse time because the marker does not survive
        // to the pass that needs it - PointerValueNode is erased by PointerAdjuster - and indexing a
        // raw pointer without it is refused, so that a bare `[` always means "ask the container"
        // (todo/B9)
        bool base_was_peeled = false;

        // the container's element contract, once the rewriter has found it. null while the base is a
        // pointer, and null while the round that would decide has not run yet
        FunctionCallExprNode *element_call = nullptr;

        // has AST::OperatorRewriter finished with this node? a null `element_call` is three different
        // states - a pointer index, a base whose type is not known yet, and one already reported as
        // an error - and only the middle one is worth asking about again. without this the fixpoint
        // would re-report every round and never converge
        //
        // false on a clone, which is right: a template body is never decided, because the operand
        // types it would be decided from are the ones substitution supplies
        bool resolution_decided = false;

        TokenReference token_bracket;

        IndexExprNode(ExprNode *base, std::vector<ExprNode *> indices, TokenReference token_bracket) :
            base(base), indices(std::move(indices)), token_bracket(token_bracket)
        {
            assert(base != nullptr && "IndexExprNode requires a base");
        };

        ~IndexExprNode() {}

        // does this bracket sit where the storage is *bound* rather than *read*? two spellings do -
        // the left of an `=`, and the operand of `&` - and they are the two the append form needs,
        // because `$a[]` names a slot that has just been grown into existence and holds nothing:
        //
        //     $a[] = 5;                 // built in the slot
        //     Point& $p = &$a[];        // the slot borrowed, to be filled field by field
        //     echo $a[];                // refused - there is nothing there to read
        //
        // a syntactic question, so the parser is what answers it. it also decides that the write is
        // an *initialization*, since a slot that holds nothing owes no teardown
        bool slot_is_bound = false;

        // true for `$a[]`, which names the slot after the last one rather than an existing element
        bool is_append() const {
            return indices.empty();
        }

        // **what is actually being indexed** - the base's type with every *transparent* level taken
        // off it, because a borrow is not part of the answer: `$a[0]` over an `Array<int32>& $a`
        // parameter indexes the array, not the pointer that reaches it
        //
        // non-nullable pointer levels are peeled and a nullable one is not, which is exactly the line
        // AST::argument_fit's t_read_through draws and for the same reason - reading through a
        // `ptr<T>` that may be null is an unchecked dereference. so a `ptr<int32>` is still a pointer
        // here, which is what sends `$p[0]` to the ':$' refusal and `$a[0]` to the element contract,
        // with no rule anywhere about which *kind* of type either one is
        //
        // one function, two askers: result_type() below and AST::OperatorRewriter. two copies would
        // be two answers to "is this a pointer index", and they would disagree exactly where it costs
        ValueType indexed_base_type() const;

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_index_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    // `[1, 2, 3]` - a bracketed list of elements, which the *destination* types.
    //
    // it has no type of its own and `result_type()` says so: a literal is a list of values, and what
    // collection they go into is decided by the storage they are written to, the same expected-type
    // rule that types every scalar literal.
    //
    // **not `Array<T>`-specific.** AST::OperatorRewriter expands one into a zero-argument constructor
    // of the destination type plus one `$dest[] = element` per element, so any type with both works -
    // which is what a `Map<K, V>` literal will reuse rather than re-derive.
    //
    // it is therefore a **statement-level** construct: it needs storage to fill and somewhere to put
    // the appends, and only the enclosing scope has both. legal as a declaration's initializer or as
    // an assignment's right-hand side, and a located error anywhere else - `f([1, 2, 3])` included,
    // because an argument has no temporary to expand into yet (todo/A13c)
    class ArrayLiteralExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_array_literal);

        std::vector<ExprNode *> elements;

        TokenReference token_bracket;

        // has AST::OperatorRewriter finished with this node? the same three-state problem
        // IndexExprNode::resolution_decided solves, and the same answer: without it a destination
        // that never becomes concrete is reported once per round
        bool expansion_decided = false;

        ArrayLiteralExprNode(std::vector<ExprNode *> elements, TokenReference token_bracket) :
            elements(std::move(elements)), token_bracket(token_bracket)
        {
        }

        ~ArrayLiteralExprNode() {}

        // **always unknown, deliberately.** the elements say what goes in, never what holds them, and
        // answering with a guess is what would let an inferred declaration latch onto a wrong type
        ValueType result_type() const override {
            return ValueType::make_unknown();
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_array_literal_expr(*this);
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