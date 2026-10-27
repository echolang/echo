#ifndef CONSTDECLNODE_H
#define CONSTDECLNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "AST/ASTVisibility.h"
#include "Lexer.h"
#include "AST/TypeNode.h"

#include <cstddef>

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

    // `const usize MAX = 100;`. no storage: ConstantExpander clones the initializer at each use.
    // a `const` variable has a `$` and a slot. child of nothing, so ownership / adjuster /
    // typecheck / codegen never see it. a closure is refused: it would capture the declaration site
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

        // raw `/** ... */` above the declaration. taken at parse so it cannot attach to the next one
        std::string doc_comment;

        // exclusive index in the module token collection, after the initializer and an optional `;`.
        // the body pass seeks here instead of skip_statement(), which would eat the next statement
        // when the semicolon was omitted (`const L = 10` then `echo L;`)
        size_t token_end = 0;

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
