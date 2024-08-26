#include "AST/ASTTypeParam.h"

#include "AST/FunctionDeclNode.h"

#include <cassert>

bool AST::TypeParamDecl::allows(const ValueType &type) const
{
    if (constraint.empty()) {
        return true;
    }

    // constraint entries are bare concrete types; compare the given type with
    // its const/pointer flags stripped so `const float` still matches `float`
    ValueType bare = type;
    bare.set_const(false);
    bare.set_pointer(false);

    for (const auto &allowed : constraint) {
        if (bare == allowed) {
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
