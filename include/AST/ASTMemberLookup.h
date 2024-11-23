#ifndef ASTMEMBERLOOKUP_H
#define ASTMEMBERLOOKUP_H

#pragma once

#include "AST/ASTValueType.h"

#include <string>
#include <vector>

namespace AST
{
    class FunctionDeclNode;

    // the candidate member functions a name denotes on `ct`, as an overload set - the member
    // counterpart of FunctionRegistry::overloads, and the only place that answers "which
    // declarations could `$obj->name(...)` be".
    //
    // an instantiation has no method table of its own. methods are declared on the template and
    // instantiated per call site by the monomorphizer, so a lookup on `Box<int32>` redirects
    // through template_ref and hands back the template's declarations - which are generic in the
    // owner's parameters, and bind them from the receiver argument. that redirect is why
    // TypeRegistry::get_or_create_instantiation needs to know nothing about members
    std::vector<FunctionDeclNode *> find_member_functions(const ComplexType *ct, const std::string &name);

    // `ct`'s destructor, or null when the type has none of its own. the same template_ref redirect
    // the overload lookup above does, and for the same reason: `Box<int32>` holds no destructor, the
    // template does, and the ownership pass hands the *template's* declaration to the drop call it
    // inserts so the monomorphizer's ordinary fixpoint instantiates it from the receiver type
    //
    // a type having no destructor does not mean it needs none - a struct whose property owns
    // something is destroyed member-wise. that question is AST::needs_destruction (ASTDestruction.h)
    FunctionDeclNode *find_destructor(const ComplexType *ct);

    // is this constructor the one that says how to copy `self_value_type`? a user-written constructor
    // whose parameter list is exactly one non-nullable borrow of its own type - `Foo&` or
    // `const Foo&` - *is* the copy constructor. nothing had to be added to the language for that: a
    // constructor is an ordinary FunctionDeclNode, a borrow parameter an ordinary parameter, and
    // `Foo($a)` an ordinary overload resolution. so this recognises the declaration that can already
    // be written rather than introducing a second construction path beside it
    //
    // asked here rather than in the parser because it is the same rule find_copy_constructor answers,
    // from the other direction - recognise versus retrieve - and one rule with two readers drifts
    //
    // `self_value_type` is the struct's self type, which for a generic is the interned
    // self-application `Box<T>`. the comparison is ValueType::operator==, which is exact, so only
    // `Box<T>&` matches and a bare `Box&` - which resolves to the template - deliberately does not;
    // the parser reports that spelling rather than silently not recognising it
    bool is_copy_constructor(const FunctionDeclNode *decl, const ValueType &self_value_type);

    // `ct`'s copy constructor, or null. the same template_ref redirect find_destructor does, and for
    // the same reason: `Box<int32>` holds no copy constructor, the template does, and the ownership
    // pass hands the *template's* declaration to the copy call it inserts so the monomorphizer's
    // ordinary fixpoint instantiates it from the receiver type
    FunctionDeclNode *find_copy_constructor(const ComplexType *ct);
};

#endif
