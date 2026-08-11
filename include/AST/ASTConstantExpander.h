#ifndef ASTCONSTANTEXPANDER_H
#define ASTCONSTANTEXPANDER_H

#pragma once

#include "AST/ASTClone.h"
#include "AST/ASTIssue.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ConstDeclNode.h"

#include <unordered_map>

namespace AST
{
    class Bundle;
    class Collector;
    class ComplexType;
    class ConstRefExprNode;
    class ExprNode;
    class File;
    class Module;
    class Namespace;
    class NamespaceManager;

    // **what a constant's name denotes**, and the one place it is answered: the exact-namespace find_symbol
    // for a name written with a prefix, the outward-walking find_symbol_in_scope for a bare one. That split
    // is the mirror rule types already follow - a qualified `geometry::Point` must not silently resolve to
    // the root's `Point`, and an unqualified name means the nearest declaration of it.
    //
    // null when nothing of that name is a constant, which includes the name being a type
    ConstDeclNode *find_constant(
        NamespaceManager &namespaces,
        const std::string &name,
        const Namespace *from,
        bool qualified
    );

    // replaces every reference to a compile-time constant with **a clone of the constant's value**.
    //
    // that is the whole semantics of a constant, and it is why there is no evaluator here: a constant does
    // not have a value, it has an expression, and each use site gets its own copy of it. Nothing in Echo
    // structurally requires a constant expression - no fixed-size arrays, no value generics, no case labels -
    // so there is nothing that would need a number rather than a tree.
    //
    // **runs before the monomorphizer's fixpoint, not inside it.** AST::OwnershipPass walks a body exactly
    // once, ever, so a reference still in a body when it gets there makes that walk's answer permanent and
    // wrong. And an expansion depends on nothing the fixpoint produces, so it converges in one pass and has
    // no business being a round of one.
    //
    // there is deliberately **no finalize()**. "Not decided yet" is not representable: this runs once over a
    // complete program, and every reference leaves it either expanded or replaced by a void after a refusal.
    // The two rewriters inside the fixpoint need that exit because being out of rounds is their proof that
    // nothing was going to answer - here, one pass *is* the proof
    class ConstantExpander : public RecursiveVisitor
    {
    public:
        ConstantExpander(Bundle &bundle);

        void run();

    protected:
        // the one seam. rewrite_place_edge is deliberately not overridden - the base routes it here, which is
        // what gets a member access's base expanded as readily as an argument
        ExprNode *rewrite_value_edge(ExprNode *expr) override;

        // tracked so `self::` can be answered: which type a body belongs to is a fact about the declaration
        // the walk is inside, and the parser could not have recorded it - AST::FunctionBodyScope carries no
        // self type, by design
        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visit_type_decl(TypeDeclNode &node) override;

    private:
        // expands one declaration's own value, so a use site clones a tree that is already expanded and a
        // chain of constants is walked once rather than once per use site. `expansion` is the memo and the
        // cycle guard both
        void expand_initializer(ConstDeclNode &decl);

        // the constant a reference names, or null with a diagnostic already collected
        ConstDeclNode *resolve(ConstRefExprNode &ref);

        // **may the module being walked name this constant at all?** A module may only name symbols from one
        // parsed before it - that is what makes a program independent of file order and what makes a library's
        // object independent of its consumers. Every other kind of name gets this for free, because resolution
        // happens while parsing and a later module has not been parsed yet; a constant is resolved after the
        // whole program is, so it is the one that has to ask
        bool module_may_name(const ConstDeclNode &decl) const;

        // the refusal half of it, collected at the reference. true when it refused, so both spellings of a
        // constant's name share one wording - the `self::` path fires in the same situation and the advice
        // about where to move the declaration is as useful there
        bool refuse_if_later_module(const ConstDeclNode &decl, const ConstRefExprNode &ref);

        // **and whether the declaration let this file name it**, which is a different question from the one
        // above: that one is about the shape of the build, this one about what the declaration said. this
        // pass is where a constant's visibility is asked for the same reason it is where the module order
        // is - a constant is the one name resolved after the whole program is parsed
        bool refuse_if_not_visible(const ConstDeclNode &decl, const ConstRefExprNode &ref);

        CodeRef code_ref_for(const TokenReference &token);
        CodeRef code_ref_at_declaration(const ConstDeclNode &decl);

        Bundle &_bundle;
        Collector &_collector;

        // parse order, which is the order a module may name another in. Built once in run()
        std::unordered_map<const Module *, size_t> _module_order;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        // what `self::` means here, null outside a type's body
        ComplexType *_current_self = nullptr;

        // held as a member because CloneContext keeps a *reference* to it. an empty substitution is the
        // identity - TypeSubstitution::lookup answers null, which substitute_type reads as "leave it"
        TypeSubstitution _no_substitution;
    };
};

#endif
