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
    class NodeCollection;
    class VarDeclNode;
    class VarRefNode;

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

    // the candidate **static** functions a name denotes on `ct` - the same overload set the lookup
    // above answers, over ComplexType::static_methods, with the same template_ref redirect.
    //
    // **why this is not AST::member_surface_namespace**, which CLAUDE.md names as the owner of "which
    // namespace holds a type's member surface" and which already carries a type's nested types and its
    // compile-time constants. Three reasons, and the first is decisive:
    //
    //  - TypeRegistry names an instantiation `result<int32, string>`, so a member surface for one would
    //    be a namespace *called* that. the surface route cannot express a generic owner at all, and a
    //    static on a generic type is the whole motivating case
    //  - FunctionRegistry::overloads walks *outward* from the namespace it is given. a member lookup
    //    does not, and must not: `Foo::f()` finding an enclosing free `f` is a bug, not a fallback
    //  - the shorthand `.f(...)` has no namespace token to name. its owner arrives as a *type*, from
    //    the destination, so an owner-keyed lookup is needed regardless - and two lookups for one
    //    concept is the recurring bug "one question, one owner" exists to stop
    //
    // so the split is by what the name denotes: the surface keeps what is arity-free, non-overloadable
    // and takes no type arguments (a nested type, a constant); this takes overload sets declared with
    // the owner's type parameters in scope
    std::vector<FunctionDeclNode *> find_static_functions(const ComplexType *ct, const std::string &name);

    // **may a value arriving here name a static owner?** - asked of a destination type, by the two
    // things that have to agree about it: Parser::parse_return's gate on whether to hand the return
    // type down as a parse-time destination at all, and AST::bind_shorthand_to, which takes the owner.
    //
    // one predicate rather than a `has_complex_type()` at each, because the *peels* are the content -
    // a borrow parameter and a `T?` both name an owner their spelling does not
    bool destination_names_a_static_owner(const ValueType &destination);

    // the owner a destination names, peeled - or `unknown` when it names none. the answer
    // destination_names_a_static_owner is a yes/no over, so the peel is written once
    ValueType static_owner_of_destination(const ValueType &destination);

    // **gives a shorthand `.f(...)` the owner its destination names.** the exact shape of
    // AST::bind_null_to beside it, and for the same reason: a value with no type of its own, typed by
    // the place it is going, at whichever of the four positions can say.
    //
    // a no-op on anything that is not an unbound shorthand, and on a destination that names no owner -
    // that second one being a *not-yet* rather than a refusal, since a parameter still mentioning a
    // type parameter says nothing about anything. the refusal, when the destination is real and simply
    // has no owner to give, belongs to the finalizing sweep, which is the only reader that knows no
    // later round is coming
    //
    // **idempotent**, like bind_array_literal_to: several passes may reach one call, and the first
    // destination to name an owner is the one that meant it
    bool bind_shorthand_to(ExprNode *expr, const ValueType &destination);

    // the shorthand this expression is *and nothing has named an owner for*, or null - the
    // tag-compare-plus-cast that keeps every asker from spelling it, null-safe on both axes.
    //
    // unbound is the whole question rather than a mode, both readers wanting a call still waiting on a
    // destination: bind_shorthand_to, whose binding an already-bound call must not have redone, and the
    // tie diagnostic, which is about an argument that has no type for the overloads to be told apart by
    FunctionCallExprNode *unbound_shorthand_call_of(ExprNode *expr);

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
    // an implicit conversion is inserted at an argument position where the user wrote nothing, so
    // it is not an `operator` - every form that grammar has is operand syntax. the spelling is
    // `#[implicit]`, and it is declared. `$a[$i]` *is* operand syntax and is a declared
    // `operator []`
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
    //
    // the arena overload is for a caller holding one without the module around it: AST::CallResolver
    // addresses an `#[implicit]` conversion's receiver while coercing, and coercion is handed the
    // NodeCollection alone. one body, so the rule stays single even where the seam differs
    ExprNode *receiver_for_member_call(NodeCollection &nodes, ExprNode *place);
    ExprNode *receiver_for_member_call(Module &module, ExprNode *place);

    // **`$local` as a place expression**, for a pass minting a call on a local it just declared. two
    // nodes and no rule of its own - it exists so the two shapes below share one spelling of it, since a
    // pass reaching for a *resolved* call on a local wants the receiver the unresolved one already builds
    // the concrete node rather than ExprNode&, so a caller building a NodeReference out of it can:
    // AST::make_ref is over a type that carries its own node_type
    VarRefNode &local_place(Module &module, VarDeclNode &local);

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

    // **a member call a pass synthesizes on a local it just minted, with the callee named rather than
    // chosen.** the receiver is `$local`, addressed through the rule above, and the call is left
    // **unresolved with `lookup_namespace` null** - which is the whole of what makes it a member call:
    // CallResolver::candidates_for reads the receiver's type off argument 0, and the fixpoint's own
    // settle_calls finishes it. A local a lowering declared is not typed until later in the round, and
    // an untyped receiver is exactly what receiver_for_member_call's `unknown` arm is for.
    //
    // it is also what makes an *erased* receiver work with no arm anywhere: find_member_functions finds
    // the requirement in the interface's own `_methods`, and ExprCodegen::gen_function_call routes on
    // FunctionDeclNode::is_interface_requirement().
    //
    // **one reader, AST::ForeachLowering**, for the three calls a cursor's own protocol declares -
    // `advance`, `current`, `key`. AST::GuardLowering was the second and is not any more: its callees
    // come off AST::UnwrapPlan now, which is the rule IterationPlan::iterate already stated. so a name
    // reaching this function is a name a *plan* did not answer, and `IterationPlan` carries only
    // `iterate`
    FunctionCallExprNode &make_unresolved_member_call(
        Module &module,
        VarDeclNode &local,
        const std::string &name,
        const TokenReference &at
    );
};

#endif
