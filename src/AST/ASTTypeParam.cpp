#include "AST/ASTTypeParam.h"

#include "AST/ASTConformance.h"
#include "AST/FunctionDeclNode.h"

#include <cassert>

bool AST::TypeParamDecl::allows(const ValueType &type) const
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
    }
    return false;
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
