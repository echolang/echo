#include "AST/ASTMemberLookup.h"

#include "AST/ASTModule.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

namespace
{
    // an instantiation carries the layout of `Box<int32>` but none of its members: they are the
    // template's declarations, instantiated per call site. so the receiver's *type* answers where to
    // look and ComplexType::template_or_self answers what is there - the null check is the only thing
    // this adds, and it is what lets every lookup below take an unchecked layout
    const AST::ComplexType *member_owner_of(const AST::ComplexType *ct)
    {
        return ct == nullptr ? nullptr : ct->template_or_self();
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

AST::FunctionDeclNode *AST::find_implicit_conversion(const AST::ValueType &from, const AST::ValueType &to)
{
    // only a declared type can offer one, and only ever to a *different* type - `t_exact` already
    // answered the identity case, and admitting it here would let a type convert to itself
    if (!from.has_complex_type() || !to.has_complex_type() || from == to) {
        return nullptr;
    }

    const AST::ComplexType *owner = member_owner_of(from.get_complex_type());

    if (owner == nullptr) {
        return nullptr;
    }

    // the published slot, not a walk of every method: this runs from the bottom of argument_fit, once
    // per candidate per argument per fixpoint round, and it used to be a string comparison against
    // every member the owner declares. a type has one or two entries here, so a flat scan of them is
    // the whole cost - and the list holds only declarations publish_implicit_conversion accepted, so
    // there is nothing left to check but the target
    for (AST::FunctionDeclNode *candidate : owner->implicit_conversions()) {
        if (candidate->get_return_type() == to) {
            return candidate;
        }
    }

    return nullptr;
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
    if (ct == nullptr) {
        return nullptr;
    }

    // the type's own slot first - see the header for why this one lookup does not simply redirect.
    // for anything but an instantiation that slot is the only one there is
    if (auto *own = ct->copy_constructor()) {
        return own;
    }

    return ct->is_instantiated() ? ct->template_ref->copy_constructor() : nullptr;
}

AST::ExprNode *AST::receiver_for_member_call(AST::Module &module, AST::ExprNode *place)
{
    if (place->result_type().is_pointer()) {
        return place;
    }

    return &module.nodes.emplace_back<AST::AddrOfExprNode>(place);
}

AST::FunctionCallExprNode &AST::make_resolved_member_call(
    AST::Module &module, AST::FunctionDeclNode *callee, const TokenReference &at, AST::ExprNode *place)
{
    auto &call = module.nodes.emplace_back<AST::FunctionCallExprNode>(
        at, std::vector<AST::ExprNode *>{ receiver_for_member_call(module, place) });

    call.decl = callee;
    call.settlement = AST::CallSettlement::t_uncoerced;

    return call;
}
