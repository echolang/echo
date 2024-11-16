#include "AST/ASTMemberLookup.h"

#include "AST/FunctionDeclNode.h"

std::vector<AST::FunctionDeclNode *> AST::find_member_functions(const AST::ComplexType *ct, const std::string &name)
{
    std::vector<AST::FunctionDeclNode *> candidates;

    if (ct == nullptr) {
        return candidates;
    }

    // an instantiation carries the layout of `Box<int32>` but none of its members: they are the
    // template's declarations, instantiated per call site. so the receiver's *type* answers where
    // to look and the template answers what is there
    const AST::ComplexType *owner = ct->is_instantiated() ? ct->template_ref : ct;

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
