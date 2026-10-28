#include "AST/ASTTypeParam.h"

#include "AST/ASTConformance.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeAliasDeclNode.h"

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
        // this is the concrete question. an atom that still names a parameter is substituted
        // first, by first_constraint_violation, and only then asked here - so Cmp<T> at
        // T = Gate is conforms_to(Gate, Cmp<Gate>) and not a second rule. a violation still
        // reports through Issue::UnsatisfiedTypeConstraint, naming constraint_spelling
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

        if (allowed.is_extern_interface_kind_constraint()) {
            const ComplexType *ct = bare.get_complex_type();
            if (ct != nullptr && ct->is_extern_interface_kind()) {
                return true;
            }
        }
    }
    return false;
}

AST::ValueType AST::intern_const_generic_token(const IntegerTokenBits &value)
{
    const uint64_t bits = integer_token_stored_bits(value);
    if (value.negative) {
        return ValueType::make_const_value(ValueTypePrimitive::t_int64, bits);
    }

    if (value.magnitude > 2147483647ull) {
        return ValueType::make_const_value(ValueTypePrimitive::t_uint64, bits);
    }

    return ValueType::make_const_value(ValueTypePrimitive::t_int32, bits);
}

AST::IntegerTokenBits AST::bits_of_const_value(const ValueType &type)
{
    IntegerTokenBits value;
    if (!type.is_const_value()) {
        return value;
    }

    const uint64_t bits = type.const_value_bits();
    if (ValueType(type.const_value_primitive()).is_signed_integer()) {
        if (static_cast<int64_t>(bits) < 0) {
            value.negative = true;
            value.magnitude = 0ull - bits;
            return value;
        }
    }

    value.magnitude = bits;
    return value;
}

bool AST::const_generic_token_fits(const ValueType &dest, const IntegerTokenBits &value)
{
    if (!dest.is_integer_type()) {
        return false;
    }

    const IntegerSize size = get_integer_size(dest.get_primitive_type());
    if (value.negative) {
        if (!size.is_signed) {
            return false;
        }

        // magnitude of the most-negative value is 2^(bits-1)
        const uint64_t limit = 1ULL << (size.bit_width() - 1);
        return value.magnitude <= limit;
    }

    return value.magnitude <= size.get_max_positive_value();
}

std::string AST::const_generic_token_refusal(const ValueType &dest, const IntegerTokenBits &value)
{
    const IntegerSize size = get_integer_size(dest.get_primitive_type());
    const std::string written = value.negative
        ? fmt::format("-{}", value.magnitude)
        : fmt::format("{}", value.magnitude);

    if (value.negative && !size.is_signed) {
        return fmt::format(
            "The literal '{}' is negative, so it cannot be a '{}'. The minimum value is '0'.",
            written,
            get_primitive_name(dest.get_primitive_type()));
    }

    if (value.negative) {
        return fmt::format(
            "The literal '{}' is too small for the integer type '{}'. The minimum value is '{}'.",
            written,
            get_primitive_name(dest.get_primitive_type()),
            size.get_max_negative_value());
    }

    return const_generic_overflow_sentence(dest, value.magnitude);
}

AST::ValueType AST::retype_const_generic(const TypeParamDecl &param, ValueType arg)
{
    if (param.is_value_param() && arg.is_const_value() && param.value_type.is_integer_type()
        && ValueType(arg.const_value_primitive()).is_integer_type()) {
        const IntegerTokenBits written = bits_of_const_value(arg);
        if (!const_generic_token_fits(param.value_type, written)) {
            return arg;
        }

        return ValueType::make_const_value(
            param.value_type.get_primitive_type(), integer_token_stored_bits(written));
    }

    return arg;
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
            if (!ValueType(type.const_value_primitive()).is_integer_type()) {
                return false;
            }

            return const_generic_token_fits(value_type, bits_of_const_value(type));
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

void AST::TypeParamDecl::set_owner(const TypeAliasDeclNode *owner)
{
    assert(_owner_type == nullptr && _owner_func == nullptr);
    _owner_alias = owner;
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
    if (_owner_alias) {
        return TypeParamOwnerKind::t_alias;
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
    if (_owner_alias) {
        return _owner_alias->name();
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
