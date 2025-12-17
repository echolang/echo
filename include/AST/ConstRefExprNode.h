#ifndef CONSTREFEXPRNODE_H
#define CONSTREFEXPRNODE_H

#pragma once

#include "AST/ExprNode.h"
#include "Lexer.h"

namespace AST
{
    class Namespace;

    // **a transient marker**: a bare identifier in an operand position, which is how a compile-time constant
    // is written - `MAX`, `std::math::PI`, `buffer::MAX`, `self::MAX`.
    //
    // it names rather than resolves, and that is the point. A constant's name is published in pass 2, which
    // runs over *every* file before any body is parsed, so a use site cannot know at parse time whether the
    // name exists yet - and a manifest module's constant is published later still. AST::ConstantExpander
    // resolves each of these and replaces it with a clone of the declaration's initializer, so nothing
    // downstream ever sees one: AST::PointerAdjuster throws on one, and codegen has no lowering for it.
    //
    // before that, result_type() answers `unknown`. That reads as "no information" rather than as a wrong
    // answer, which is the same standing an unsettled call has - a declaration initialized from one takes
    // its type after the expansion, from the expression that landed there.
    class ConstRefExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_const_ref);

        // the identifier, which is both the name to look up and where a diagnostic points
        TokenReference token_name;

        // where to look: the namespace a qualified name named, or the one the reference was *written* in for
        // a bare name, which then resolves outward from there by the mirror rule types already follow.
        //
        // recorded here rather than only used at the parse site for FunctionCallExprNode::lookup_namespace's
        // reason - the lookup has to be repeatable from a pass that has no parser context left
        const Namespace *lookup_namespace = nullptr;

        // was the name written with a namespace prefix? the two take *different* lookups - the
        // exact-namespace find_symbol for a qualified name, the outward-walking find_symbol_in_scope for a
        // bare one - so this cannot be derived from lookup_namespace being set
        bool is_qualified = false;

        // written `self::MAX`, meaning "the type this reference sits inside". Resolved by the expander from
        // the enclosing declaration rather than at parse time, because a method body has no way back to its
        // owner: AST::FunctionBodyScope deliberately carries no self type
        bool is_self_qualified = false;

        ConstRefExprNode(TokenReference token_name, const Namespace *lookup_namespace, bool is_qualified) :
            token_name(token_name), lookup_namespace(lookup_namespace), is_qualified(is_qualified)
        {};

        ~ConstRefExprNode() {};

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_const_ref(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
