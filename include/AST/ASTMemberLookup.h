#ifndef ASTMEMBERLOOKUP_H
#define ASTMEMBERLOOKUP_H

#pragma once

#include "AST/ASTValueType.h"

#include "Token.h"

#include <string>
#include <vector>

namespace AST
{
    class ExprNode;
    class FunctionCallExprNode;
    class FunctionDeclNode;
    class Module;

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
    // `#[implicit]`, whose return type is exactly `to`. Exact rather than convertible on purpose, so a
    // chain of conversions can never be searched for.
    //
    // Everything else about a candidate - it takes no parameters, it is a method, its target is a
    // declared type that owns nothing - was decided at the declaration by Parser's
    // publish_implicit_conversion. That is what lets this be a comparison rather than a filter.
    //
    // This used to recognise the conversion by the member's *spelling*, a published `view_method_name`
    // constant the compiler compared every method against. The comment there said operator overloading
    // would eventually replace it with a declarable operator, and **that was wrong**.
    //
    // Every form the operator grammar has is operand syntax the user writes - infix, prefix, suffix,
    // each with operands and a precedence. An implicit conversion is inserted at an argument position
    // where the user wrote nothing at all. There is no operand to hang it on, so `operator` could never
    // have taken it over, and the mislabel is what let a magic name look temporary. The spelling is
    // `#[implicit]`, and it is declared.
    //
    // The **bracket** is the genuinely different case, and it did turn out to be the operator grammar's
    // to take: `$a[$i]` *is* operand syntax, and it is a declared `operator []` now
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

    // is this constructor the one that says how to copy `self_value_type`?
    //
    // A user-written constructor whose parameter list is exactly one non-nullable borrow of its own type
    // - `Foo&` or `const Foo&` - *is* the copy constructor. Nothing had to be added to the language for
    // that. A constructor is an ordinary FunctionDeclNode, a borrow parameter an ordinary parameter, and
    // `Foo($a)` an ordinary overload resolution. So this recognises the declaration that can already be
    // written, rather than introducing a second construction path beside it.
    //
    // Asked here rather than in the parser, because it is the same rule find_copy_constructor answers
    // from the other direction - recognise versus retrieve - and one rule with two readers drifts.
    //
    // `self_value_type` is the struct's self type, which for a generic is the interned self-application
    // `Box<T>`. The comparison is ValueType::operator==, which is exact, so only `Box<T>&` matches and a
    // bare `Box&` - which resolves to the template - deliberately does not. The parser reports that
    // spelling rather than silently not recognising it
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

    // **argument 0 of a synthesized member call: the place, addressed unless it already is an address.**
    // the parameter is the borrow `Foo&`, so a value ranked against it would be no fit at all - but a
    // place that is *already* a pointer must be handed over bare. a synthesized class deinit's `$this` is
    // one, and so is a borrow parameter a `foreach` iterates. addressing one of those twice hands the
    // callee a `ptr<ptr<Foo>>`, which unifies against nothing: the call is then **never instantiated at
    // all, silently**, and only the type checker notices, far away. one function, because a rule whose
    // failure mode is silence must not have two copies of itself to keep in step - and it is asked of the
    // *expression*, so a receiver whose declaration is not typed yet answers `unknown` and gets addressed
    ExprNode *receiver_for_member_call(Module &module, ExprNode *place);

    // **a member call a pass synthesizes, with its callee already chosen.** a member call is a
    // FunctionCallExprNode with the receiver prepended - there is no other machinery - so all this owns is
    // the rule above and the settlement below.
    //
    // the call is published as **resolved but uncoerced**: there is no name to look up and no overload
    // set to search, and for an instantiation `callee` is the *template's* declaration - the
    // monomorphizer's next round binds the owner's parameters from the receiver and rewires the call to
    // the instance. fitting the receiver to the borrow parameter stays AST::CallResolver's, in a later
    // round, which is why both callers run inside that fixpoint
    FunctionCallExprNode &make_resolved_member_call(
        Module &module,
        FunctionDeclNode *callee,
        const TokenReference &at,
        ExprNode *place
    );
};

#endif
