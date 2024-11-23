#include "AST/ASTMemberLookup.h"

#include "AST/FunctionDeclNode.h"

namespace
{
    // an instantiation carries the layout of `Box<int32>` but none of its members: they are the
    // template's declarations, instantiated per call site. so the receiver's *type* answers where to
    // look and the template answers what is there.
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
