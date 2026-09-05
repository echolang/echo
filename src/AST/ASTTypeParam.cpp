#include "AST/ASTTypeParam.h"

#include "AST/ASTConformance.h"
#include "AST/FunctionDeclNode.h"

#include <cassert>
#include <fmt/core.h>

bool AST::constraint_admits(const std::vector<ValueType> &constraint, const ValueType &type)
{
    if (constraint.empty()) {
        return true;
    }

    // constraint entries are bare concrete types; compare with const stripped so `const float`
    // still matches `float`. pointerness is deliberately NOT stripped - `Vec<T: numeric>` must
    // reject `Vec<ptr<int32>>`, and the decay that makes a pointer argument bind a bare T is a
    // call-boundary rule that lives in Monomorphizer::unify, not a constraint rule
    ValueType bare = ValueType::make_mutable(type);

    for (const auto &allowed : constraint) {
        if (bare == allowed) {
            return true;
        }

        // **the one arm interfaces needed.** an interface atom is satisfied by conformance rather than
        // by identity - it names a capability, and the set of types answering it is open, which is
        // exactly what a concrete-set constraint could never express
        //
        // this is the whole of it: first_constraint_violation, can_instantiate, unify_type,
        // match_function and Monomorphizer::determine_type_args are unchanged, because "does this
        // argument satisfy this parameter's constraint" already had one owner and this is it. a
        // violation still reports through Issue::UnsatisfiedTypeConstraint, naming
        // constraint_spelling - which for an interface reads as the interface's own name
        if (allowed.is_interface() && AST::conforms_to(bare, allowed)) {
            return true;
        }

        // **the kind predicate**, open the way an interface is: there is no finite set of
        // classes to expand `numeric`-style, and `Box<int32>` is a class the author of the
        // constraint has never named. `is_class()` on the argument is the whole of it —
        // an instantiation carries the template's kind, so a generic class answers yes
        if (allowed.is_class_kind_constraint() && bare.is_class()) {
            return true;
        }
    }
    return false;
}

bool AST::const_generic_bits_fit(const ValueType &dest, uint64_t bits)
{
    if (!dest.is_integer_type()) {
        return false;
    }

    return bits <= get_integer_size(dest.get_primitive_type()).get_max_positive_value();
}

std::string AST::const_generic_overflow_sentence(const ValueType &dest, uint64_t bits)
{
    const IntegerSize size = get_integer_size(dest.get_primitive_type());
    return fmt::format(
        "The literal '{}' is too large for the integer type '{}'. The maximum value is '{}'.",
        bits,
        get_primitive_name(dest.get_primitive_type()),
        size.get_max_positive_value());
}

bool AST::TypeParamDecl::allows(const ValueType &type) const
{
    if (param_kind == TypeParamKind::t_value) {
        if (type.is_const_value()) {
            return ValueType(type.const_value_primitive()).is_integer_type()
                && const_generic_bits_fit(value_type, type.const_value_bits());
        }

        if (type.is_type_param() && type.get_type_param()->is_value_param()) {
            return type.get_type_param()->value_type.is_integer_type()
                && value_type.is_integer_type();
        }

        return false;
    }

    if (type.is_const_value()) {
        return false;
    }

    if (type.is_type_param() && type.get_type_param()->is_value_param()) {
        return false;
    }

    return AST::constraint_admits(constraint, type);
}

void AST::TypeParamDecl::set_owner(ComplexType *owner)
{
    assert(_owner_func == nullptr);
    _owner_type = owner;
}

void AST::TypeParamDecl::set_owner(const FunctionDeclNode *owner)
{
    assert(_owner_type == nullptr);
    _owner_func = owner;
}

AST::TypeParamOwnerKind AST::TypeParamDecl::owner_kind() const
{
    // ahead of the t_type arm: an associated type's owner *is* a ComplexType, so the flag is the only
    // thing that tells the two apart
    if (is_associated) {
        return TypeParamOwnerKind::t_associated;
    }
    if (_owner_type) {
        return TypeParamOwnerKind::t_type;
    }
    if (_owner_func) {
        return TypeParamOwnerKind::t_function;
    }
    return TypeParamOwnerKind::t_none;
}

std::string AST::TypeParamDecl::owner_name() const
{
    if (_owner_type) {
        return _owner_type->name.value_or("");
    }
    if (_owner_func) {
        return _owner_func->func_name();
    }
    return "";
}

std::string AST::TypeParamDecl::describe() const
{
    std::string owner = owner_name();
    if (owner.empty()) {
        return name;
    }
    return owner + "::" + name;
}

AST::TypeParamDecl *AST::TypeParamRegistry::declare(const std::string &name, size_t ordinal, std::optional<TokenReference> name_token)
{
    _owned.push_back(std::make_unique<TypeParamDecl>(name, ordinal, name_token));
    return _owned.back().get();
}
