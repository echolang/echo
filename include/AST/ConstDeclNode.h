#ifndef CONSTDECLNODE_H
#define CONSTDECLNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "AST/ASTVisibility.h"
#include "Lexer.h"
#include "AST/TypeNode.h"

namespace AST
{
    class ExprNode;
    class ComplexType;

    // how far AST::ConstantExpander has got with one declaration's own initializer. one field serving as
    // both the memo and the cycle guard, because they are the same question asked at two moments: a
    // constant already being expanded is a constant defined in terms of itself
    enum class ConstExpansion
    {
        t_pending,
        t_expanding,
        t_expanded,
        t_refused,
    };

    // a **compile-time constant**: `const usize MAX_CAPACITY = 100;`, declared at file scope, in a
    // namespace, or in a struct body. Not a variable, and deliberately not a VarDeclNode with a bit set.
    //
    // the difference is the whole feature. A `const` *variable* carries a `$`, has storage, and lives in
    // the scope it was written in - a block, if that is where it was written. A constant has no storage at
    // all: AST::ConstantExpander replaces every reference to it with a **clone of this initializer**, so
    // what a use site gets is the expression, not a value. That is what lets one live in a library module,
    // where a file-scope variable is silently dropped for want of anywhere to put it, and it is why the
    // name has no `$`: `$` is how this language spells a value with storage.
    //
    // the initializer may be any expression, which follows from copying it rather than evaluating it - and
    // so does the cost, since a call in one runs once per use site. Two shapes are refused at the
    // declaration: one naming a variable, because `$x` does not exist where the copy lands, and a closure
    // literal, because a closure captures where it is *written* and a constant is copied to where it is
    // *used*.
    //
    // **this node is a child of nothing.** The arena owns it and no file root holds it, which is what keeps
    // it out of AST::OwnershipPass's once-only walk, out of AST::PointerAdjuster, out of AST::TypeChecker
    // and out of codegen - none of which has anything to say about a declaration with no storage. The
    // consequence to know is that the monomorphizer's arena-wide sweeps over FunctionCallExprNode still see
    // an *unused* constant's initializer, so a misspelled function in one is still reported. That is
    // harmless, and it is the reason a closure literal is refused rather than merely discouraged: a
    // FunctionDeclNode in the arena becomes a real symbol with a body request.
    class ConstDeclNode : public Node
    {
        // null when the declaration wrote no type - `const MAX = 100;` - in which case the initializer's
        // own type is what every use site sees
        TypeNode *_type_node = nullptr;

    public:

        // the bare identifier naming the constant. `t_identifier`, never `t_varname`, which is the one
        // token that tells this apart from a variable declaration - see Parser::starts_constdecl
        TokenReference token_name;

        // what every reference to this constant is replaced by, cloned once per use site
        ExprNode *value = nullptr;

        // the struct or class this constant belongs to, null outside one. What `self::MAX` resolves against,
        // and the reason a constant in a *generic* type is refused: there is no substitution for its `T`
        // by the time the expander runs
        ComplexType *owner = nullptr;

        // where this was written, which two questions read for two different reasons. the *file*, so a
        // diagnostic about the declaration renders the right excerpt - this node is reachable from the arena
        // alone, so no pass can name it from the walk that reached it, and a cycle is reported at the
        // declaration rather than at whichever use site happened to close the loop. and the *module*,
        // because a module may only name symbols from one parsed before it, which parse-time resolution
        // enforces everywhere else by construction - a reference expanded after the whole program is parsed
        // has to check it, or a library could hold a clone of an application's expression in its own arena
        //
        // AST::DeclarationOrigin is the same pair on every declaration that carries one, so the
        // visibility rules ask one shape rather than three
        DeclarationOrigin declared_in;

        // **who may name this constant.** the file and module axes only: a constant declared in a struct body
        // is `self::MAX`, reached through the owner's namespace, and a `private` there would be the *member*
        // axis - which the parser refuses rather than silently reading as this one
        Visibility visibility = Visibility::t_public;

        ConstExpansion expansion = ConstExpansion::t_pending;

        ConstDeclNode(TokenReference token_name, TypeNode *type) :
            _type_node(type), token_name(token_name)
        {};

        ~ConstDeclNode() {};

        ECO_AST_NODE_TYPE(n_const_decl);

        const std::string &name() const {
            return token_name.value();
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_const_decl(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
