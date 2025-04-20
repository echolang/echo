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

    // there is deliberately no find_member_type here. a nested type is only ever reached through an
    // owner *named in source*, which is a declaration and never an instantiation, so there is nothing
    // for the template_ref redirect below to do - ComplexType::find_member_type_decl is the whole
    // lookup, and it hands back the declaration its three callers actually want. that also lines up
    // with the refusal of a nested type inside a *generic* owner at its declaration site: one
    // `Iterator` shared by `Box<int32>` and `Box<float>` is the right answer only while it cannot
    // mention `T`, and deciding that per instantiation is what TypeRegistry::derive_instantiation
    // would have to grow - it substitutes an instantiation's properties and conformances and knows
    // nothing about member types

    // the method that implicitly converts a `from` into a `to`, or null when the type declares none.
    //
    // **the one rule** for "does this type convert to that one": a method its owner marked
    // `#[implicit]`, whose return type is exactly `to`. exact rather than convertible on purpose, so a
    // chain of conversions can never be searched for. everything else about a candidate - it takes no
    // parameters, it is a method, its target is a declared type that owns nothing - was decided at the
    // declaration by Parser's publish_implicit_conversion, which is what lets this be a comparison
    // rather than a filter
    //
    // this used to recognise the conversion by the member's *spelling*, a published `view_method_name`
    // constant the compiler compared every method against. the comment there said operator overloading
    // would eventually replace it with a declarable operator, and **that was wrong**: every form the
    // operator grammar has is operand syntax the user writes - infix, prefix, suffix, each with
    // operands and a precedence - while an implicit conversion is inserted at an argument position
    // where the user wrote nothing at all. there is no operand to hang it on, so `operator` could
    // never have taken it over, and the mislabel is what let a magic name look temporary. the spelling
    // is `#[implicit]` and it is declared. (the **bracket** is the genuinely different case, and it
    // did turn out to be the operator grammar's to take: `$a[$i]` *is* operand syntax, and it is a
    // declared `operator []` now)
    //
    // three readers, mirroring how argument_fit is already consumed: AST::argument_fit ranks it (as
    // t_declared_conversion, below every built-in conversion, so an overload taking the owning type
    // always wins), TypeChecker accepts it, and AST::CallResolver is the one that inserts the call
    FunctionDeclNode *find_implicit_conversion(const ValueType &from, const ValueType &to);

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

    // `ct`'s copy constructor, or null. the type's **own slot first**, and only then the template_ref
    // redirect find_destructor does - the one lookup here that does not simply redirect
    //
    // a **written** copy constructor is a member like any other and lives on the template, so an
    // instantiation reaches it through the redirect and the monomorphizer's ordinary fixpoint
    // instantiates it from the receiver type. a **synthesized** one is per instantiation, like a
    // class's deinit, because whether the compiler can write it at all depends on the concrete
    // property types: `Box<Handle>` is a retain per field and `Box<Buffer>` has no copy at all. so an
    // instance's slot is not a cache of the template's - it holds a different declaration, and it wins
    //
    // the two can never both be set: AST::copy_is_synthesizable (ASTCopy.h) declines a type this very
    // lookup already answers for, and it is the only gate OwnershipPass synthesizes behind
    FunctionDeclNode *find_copy_constructor(const ComplexType *ct);
};

#endif
