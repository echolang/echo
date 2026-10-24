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

    // a constant's name: exact-namespace when qualified, outward when bare. null if the name is not one
    ConstDeclNode *find_constant(
        NamespaceManager &namespaces,
        const std::string &name,
        const Namespace *from,
        bool qualified
    );

    // each use of a constant becomes a clone of its initializer. before the fixpoint: OwnershipPass
    // walks a body once, so a leftover reference makes that walk's answer permanent. no finalize()
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

        // the RHS of a named map lives on File::enum_maps, not as an edge of any function until
        // mint clones it. rewrite those expressions here so check_enum_maps folds the same tree
        // ConstantExpander already settled
        void expand_enum_map_values(Module &module);

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
