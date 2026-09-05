#ifndef ASTTYPEPARAM_H
#define ASTTYPEPARAM_H

#pragma once

#include "AST/ASTValueType.h"
#include "Token.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class FunctionDeclNode;

    // which kind of declaration introduced a type parameter. derived from whichever owner
    // pointer is set, never stored, so the two can never disagree
    enum class TypeParamOwnerKind
    {
        t_none,
        t_type,
        t_function,

        // an interface's associated type - `type Iter : contract::iterator<V>`. its owner is a ComplexType like
        // t_type's, so this one is read off TypeParamDecl::is_associated rather than off which owner
        // pointer is set
        t_associated,
    };

    // **the constraint rule**, stated over a constraint list rather than over a declaration.
    //
    // extracted from TypeParamDecl::allows, which is now its one-line caller, so that an *associated*
    // type can be checked against its constraint substituted through the conformance: `type Iter :
    // contract::iterator<V>` means nothing until `V` is bound, and `allows` reads `this->constraint` verbatim
    //
    // still the sole owner of "is this type argument allowed" - two callers, one rule
    bool constraint_admits(const std::vector<ValueType> &constraint, const ValueType &type);

    // do these bits sit inside `dest`, a value parameter's integer type. asked by TypeParamDecl::allows
    // so a `tiny<300>` over `const uint8 N` is refused at the argument rather than interned as bits
    // the body cannot hold
    bool const_generic_bits_fit(const ValueType &dest, uint64_t bits);

    // the sentence TypeParamDecl::allows' reporters use, matching AST::type_literal_at's overflow wording
    std::string const_generic_overflow_sentence(const ValueType &dest, uint64_t bits);

    // a parameter is a type, or a compile-time value. the split is a kind rather than a flag so
    // a site that meant `T` cannot silently accept `10`, and so `fixed_array<int32, 10>` and
    // `fixed_array<int32, 11>` intern as two types because their second argument's bits differ
    enum class TypeParamKind
    {
        t_type,
        t_value,
    };

    // one generic type parameter as written at its declaration site - the `T` in `struct Box<T>`
    // or `function twice<T>(...)`, and the `N` in `struct fixed_array<T, const usize N>`. a
    // ValueType of kind t_generic refers to one of these, so the name, the declaration token and
    // any constraint travel with every use of the parameter instead of having to be recovered
    // from the owner by index
    //
    // owned by a TypeParamRegistry, referenced everywhere else by raw pointer. that is forced,
    // not preferred: CloneContext::shallow copy-constructs nodes and ComplexType is copy-assigned,
    // so a unique_ptr member on either would break all cloning
    class TypeParamDecl
    {
    public:
        // the name the user wrote
        std::string name;

        // the declaration site, so a diagnostic can point at where `T` was introduced rather
        // than only at the use that went wrong
        std::optional<TokenReference> name_token;

        // position within its own owner's parameter list
        size_t ordinal = 0;

        // the set of concrete types this parameter may be substituted with. empty means
        // unconstrained. aliases (e.g. `numeric`) are expanded into this set at parse time,
        // so checking is exact set membership. `class` is not expanded: it is one
        // ValueTypeKind::t_kind_class atom, and constraint_admits asks is_class() of the
        // argument rather than enumerating types
        std::vector<ValueType> constraint;

        // the original constraint source (e.g. "numeric|bool"), kept for diagnostics so an
        // error can name what the user actually wrote
        std::string constraint_spelling;

        // **is this an interface's associated type** rather than one of its type parameters?
        //
        // stored rather than derived ("my owner's type_parameters does not contain me") because the
        // derivation is a linear scan on a hot path, and because a second way to ask is a second way to
        // get a different answer. stamped by ComplexType::add_associated_type, the single minter, exactly
        // as add_type_parameter stamps ordinal and owner - so the two cannot disagree
        bool is_associated = false;

        // `t_type` is `<T>`, `t_value` is `<const usize N>`. a value parameter is constrained by
        // its *value type* rather than by a `: numeric`-style atom list, and a use of `N` in an
        // expression is a compile-time integer, not a type
        TypeParamKind param_kind = TypeParamKind::t_type;

        // meaningful iff `param_kind == t_value`. the type `N` has as a value, typically `usize`
        ValueType value_type;

        TypeParamDecl(std::string name, size_t ordinal, std::optional<TokenReference> name_token) :
            name(std::move(name)),
            name_token(name_token),
            ordinal(ordinal)
        {}

        ~TypeParamDecl() {};

        bool is_constrained() const {
            return !constraint.empty();
        }

        bool is_value_param() const {
            return param_kind == TypeParamKind::t_value;
        }

        // true if `type` satisfies the constraint (always true when unconstrained)
        // const/pointer flags on `type` are ignored
        //
        // a value parameter admits a `t_const_value` whose bits fit `value_type` (or another value
        // parameter of an integer type), and refuses a type. a type parameter refuses a
        // `t_const_value`, even when unconstrained - otherwise `Box<4>` would intern as a type
        // whose argument is a number
        bool allows(const ValueType &type) const;

        // the declaring struct/class template. asserts no function owner is set
        void set_owner(ComplexType *owner);

        // the declaring generic function. asserts no type owner is set
        void set_owner(const FunctionDeclNode *owner);

        TypeParamOwnerKind owner_kind() const;

        ComplexType *owner_type() const {
            return _owner_type;
        }

        const FunctionDeclNode *owner_func() const {
            return _owner_func;
        }

        // the declaring template's name ("Box", "twice"), empty when no owner is set
        std::string owner_name() const;

        // the parameter qualified by its owner ("Box::T"), the bare name when there is no
        // owner. for diagnostics only - ValueType::get_type_desciption() stays unqualified
        // because it feeds the interned name of every generic application
        std::string describe() const;

    private:
        // exactly one of these is set, which is what owner_kind() reads
        ComplexType *_owner_type = nullptr;
        const FunctionDeclNode *_owner_func = nullptr;
    };

    // owns every type-parameter declaration in a bundle. append-only: declarations stranded by
    // parser error recovery stay until the registry dies, which is cheap and keeps every handed
    // out pointer stable for the registry's lifetime
    class TypeParamRegistry
    {
    public:
        TypeParamRegistry() = default;
        TypeParamRegistry(const TypeParamRegistry &) = delete;
        TypeParamRegistry &operator=(const TypeParamRegistry &) = delete;

        TypeParamDecl *declare(const std::string &name, size_t ordinal, std::optional<TokenReference> name_token = std::nullopt);

    private:
        std::vector<std::unique_ptr<TypeParamDecl>> _owned;
    };
};

#endif
