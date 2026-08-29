#ifndef ASTCONSTRUCTION_H
#define ASTCONSTRUCTION_H

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace AST
{
    class Collector;
    class FunctionDeclNode;
    class Module;
    class ScopeNode;
    class TypeDeclNode;
    class VarDeclNode;

    // property names `$this` is assigned on every completing path of `body`. completing means
    // control falls out of the bottom or `return`s; a `die` path does not complete. asked of an
    // `init` body to decide which fields are derived, and of a constructor body to decide which
    // fields that constructor still owes
    std::unordered_set<std::string> fields_assigned_on_all_paths(
        const ScopeNode *body,
        const VarDeclNode *self
    );

    // instance properties `init` assigns on every completing path. empty when the type has no
    // `init`, or when nothing is proven. those fields are omitted from the implicit constructor
    std::unordered_set<std::string> derived_fields_of(const TypeDeclNode &type);

    // **one construction check, three facts.** implicit ctor assigns every non-derived field;
    // each user ctor assigns every non-derived field `init` does not; `init` assigns every derived
    // field on all paths and reads only fields the preceding ctor assigned (or that `init` itself
    // has already assigned on this path). reports InitAssignsOnSomePaths,
    // ConstructionLeavesFieldUnassigned, InitReadsUnassignedField. asked of the type after bodies
    // exist - TypeChecker::visit_type_decl, including on a generic template
    void check_construction(TypeDeclNode &type, Collector &collector, Module &module);
};

#endif
