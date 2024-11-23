#include "AST/ASTMemberLookup.h"

#include "AST/FunctionDeclNode.h"

namespace
{
    // an instantiation carries the layout of `Box<int32>` but none of its members: they are the
    // template's declarations, instantiated per call site. so the receiver's *type* answers where to
    // look and the template answers what is there
    //
    // the one spelling of that redirect, because it is the contract that keeps TypeRegistry ignorant
    // of members - both lookups below are the same rule asked about a different store
    const AST::ComplexType *member_owner_of(const AST::ComplexType *ct)
    {
        if (ct == nullptr) {
            return nullptr;
        }

        return ct->is_instantiated() ? ct->template_ref : ct;
    }
}

std::vector<AST::FunctionDeclNode *> AST::find_member_functions(const AST::ComplexType *ct, const std::string &name)
{
    std::vector<AST::FunctionDeclNode *> candidates;

    const AST::ComplexType *owner = member_owner_of(ct);

    if (owner == nullptr) {
        return candidates;
    }

    // the name token rather than func_name(), which answers a std::string *by value*: this runs for
    // every method the owner declares, on every member call site
    for (auto *method : owner->methods()) {
        if (method->name_token.has_value() && method->name_token.value().value() == name) {
            candidates.push_back(method);
        }
    }

    return candidates;
}

AST::FunctionDeclNode *AST::find_destructor(const AST::ComplexType *ct)
{
    const AST::ComplexType *owner = member_owner_of(ct);

    return owner == nullptr ? nullptr : owner->destructor();
}

bool AST::is_copy_constructor(const AST::FunctionDeclNode *decl, const AST::ValueType &self_value_type)
{
    // exactly one parameter, and args[0] *is* it: a constructor's `$this` is a body-local of value
    // type rather than a receiver argument, so there is no implicit_arg_count() offset to apply here
    // the way a method's or a destructor's would need one
    if (decl == nullptr || !decl->is_constructor() || decl->args.size() != 1) {
        return false;
    }

    const AST::ValueType param = decl->parameter_type(0);

    // a borrow, which is a pointer that cannot be null. `ptr<Foo>` is a legitimately different
    // constructor - it takes an address that may be nothing - and must not be captured
    if (!param.is_pointer() || param.is_nullable()) {
        return false;
    }

    // const dropped on both sides, so `const Foo&` is recognised too. that is the natural spelling
    // for a copy, and it costs nothing at the call site: is_implicitly_convertible already accepts a
    // `Foo&` argument for a `const Foo&` parameter, so the plain borrow the ownership pass builds
    // needs no cast. it does mean `Foo&` and `const Foo&` are two *different* signatures that both
    // answer here, which is why the parser reports the second one itself - register_function sees no
    // duplicate
    return AST::ValueType::make_mutable(param.pointee()) == AST::ValueType::make_mutable(self_value_type);
}

AST::FunctionDeclNode *AST::find_copy_constructor(const AST::ComplexType *ct)
{
    const AST::ComplexType *owner = member_owner_of(ct);

    return owner == nullptr ? nullptr : owner->copy_constructor();
}
