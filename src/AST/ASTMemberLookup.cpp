#include "AST/ASTMemberLookup.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTModule.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

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

std::vector<AST::FunctionDeclNode *> AST::find_static_functions(const AST::ComplexType *ct, const std::string &name)
{
    std::vector<AST::FunctionDeclNode *> candidates;

    const AST::ComplexType *owner = member_owner_of(ct);

    if (owner == nullptr) {
        return candidates;
    }

    // deliberately not asking what *kind* `owner` is. a lookup is a retrieval, and whether a `static`
    // may be declared on a given kind was already answered where it was written - which is what will
    // let an enum's case constructors be read out of this list with nothing here to change
    for (auto *fn : owner->static_methods()) {
        if (fn->name_token.has_value() && fn->name_token.value().value() == name) {
            candidates.push_back(fn);
        }
    }

    return candidates;
}

bool AST::destination_names_a_static_owner(const AST::ValueType &destination)
{
    return static_owner_of_destination(destination).has_complex_type();
}

AST::ValueType AST::static_owner_of_destination(const AST::ValueType &destination)
{
    // the borrow peel is AST::parameter_auto_borrows', the one predicate that owns "does this
    // parameter position take an address", and the nullable one follows it: what a `result<T, E>& $r`
    // parameter receives is a `result<T, E>` the caller materialises, and what a `result<T, E>? $r`
    // receives is the payload's - the optional's own wrapper declares nothing.
    //
    // deliberately not AST::implicit_conversion_target, which is those same peels plus a make_mutable:
    // this answer is a call's `static_owner` rather than a comparison, so the const it was written
    // with is part of the type it names
    ValueType wanted = destination;

    if (parameter_auto_borrows(wanted)) {
        wanted = wanted.pointee();
    }

    if (wanted.is_nullable()) {
        wanted = ValueType::make_non_nullable(wanted);
    }

    // a type parameter, a `void`, an `unknown` - nothing has said what this is yet, and answering
    // "no owner" for one would turn a not-yet into a refusal
    if (!wanted.has_complex_type()) {
        return ValueType::make_unknown();
    }

    return wanted;
}

AST::FunctionCallExprNode *AST::unbound_shorthand_call_of(AST::ExprNode *expr)
{
    if (expr == nullptr || expr->get_node_type() != AST::NodeType::n_expr_call) {
        return nullptr;
    }

    auto *call = static_cast<AST::FunctionCallExprNode *>(expr);

    if (!call->is_shorthand_static_call() || call->static_owner.has_complex_type()) {
        return nullptr;
    }

    return call;
}

bool AST::bind_shorthand_to(AST::ExprNode *expr, const AST::ValueType &destination)
{
    auto *call = unbound_shorthand_call_of(expr);

    if (call == nullptr) {
        return false;
    }

    const ValueType owner = static_owner_of_destination(destination);

    if (!owner.has_complex_type()) {
        return false;
    }

    call->static_owner = owner;

    // back to unresolved so the next round looks the name up against the owner it now has. the earlier
    // rounds answered t_unknown_name, which settle() deliberately leaves non-terminal for exactly this
    call->settlement = CallSettlement::t_unresolved;

    return true;
}

AST::FunctionDeclNode *AST::find_outbound_implicit_conversion(
    const AST::ComplexType *owner,
    const AST::ValueType &to
)
{
    owner = member_owner_of(owner);

    if (owner == nullptr) {
        return nullptr;
    }

    for (AST::FunctionDeclNode *candidate : owner->implicit_conversions()) {
        if (candidate->has_receiver() && candidate->get_return_type() == to) {
            return candidate;
        }
    }

    return nullptr;
}

AST::FunctionDeclNode *AST::find_inbound_implicit_conversion(
    const AST::ComplexType *owner,
    const AST::ValueType &from
)
{
    owner = member_owner_of(owner);

    if (owner == nullptr) {
        return nullptr;
    }

    // a comparison: publish already refused a borrow parameter, so the source is the
    // declared type and implicit_conversion_source is what peels a T& argument first
    for (AST::FunctionDeclNode *candidate : owner->implicit_conversions()) {
        if (!candidate->has_receiver()
            && candidate->args.size() == 1
            && candidate->parameter_type(0) == from) {
            return candidate;
        }
    }

    return nullptr;
}

AST::FunctionDeclNode *AST::find_implicit_conversion(const AST::ValueType &from, const AST::ValueType &to)
{
    // never to itself - `t_exact` already answered the identity case, and admitting it here would
    // let a type convert to itself. inbound and outbound both refuse that at the declaration too
    if (from == to) {
        return nullptr;
    }

    // the published slot, not a walk of every method: this runs from the bottom of argument_fit, once
    // per candidate per argument per fixpoint round. a type has one or two entries here, so a flat
    // scan of them is the whole cost - and the list holds only declarations
    // publish_implicit_conversion accepted, so there is nothing left to check but the target (outbound)
    // or the source (inbound)
    if (from.has_complex_type()) {
        if (auto *found = find_outbound_implicit_conversion(from.get_complex_type(), to)) {
            return found;
        }
    }

    if (to.has_complex_type()) {
        return find_inbound_implicit_conversion(to.get_complex_type(), from);
    }

    return nullptr;
}

AST::FunctionDeclNode *AST::find_destructor(const AST::ComplexType *ct)
{
    const AST::ComplexType *owner = member_owner_of(ct);

    return owner == nullptr ? nullptr : owner->destructor();
}

AST::FunctionDeclNode *AST::find_init(const AST::ComplexType *ct)
{
    const AST::ComplexType *owner = member_owner_of(ct);

    return owner == nullptr ? nullptr : owner->type_init();
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

AST::ExprNode *AST::receiver_for_member_call(AST::NodeCollection &nodes, AST::ExprNode *place)
{
    if (place->result_type().is_pointer()) {
        return place;
    }

    return &nodes.emplace_back<AST::AddrOfExprNode>(place);
}

AST::ExprNode *AST::receiver_for_member_call(AST::Module &module, AST::ExprNode *place)
{
    return receiver_for_member_call(module.nodes, place);
}

AST::VarRefNode &AST::local_place(AST::Module &module, AST::VarDeclNode &local)
{
    auto &var = module.nodes.emplace_back<AST::VarNode>(&local, local.token_varname);

    return module.nodes.emplace_back<AST::VarRefNode>(&var);
}

AST::FunctionCallExprNode &AST::make_resolved_member_call(
    AST::Module &module,
    AST::FunctionDeclNode *callee,
    const TokenReference &at,
    AST::ExprNode *place
)
{
    auto &call = module.nodes.emplace_back<AST::FunctionCallExprNode>(
        at, std::vector<AST::ExprNode *>{ receiver_for_member_call(module, place) });

    call.decl = callee;
    call.settlement = AST::CallSettlement::t_uncoerced;

    return call;
}

AST::FunctionCallExprNode &AST::make_unresolved_member_call(
    AST::Module &module,
    AST::VarDeclNode &local,
    const std::string &name,
    const TokenReference &at
)
{
    return module.nodes.emplace_back<AST::FunctionCallExprNode>(
        module.make_virtual_token(name, Token::Type::t_identifier, at),
        std::vector<AST::ExprNode *>{
            receiver_for_member_call(module, &local_place(module, local)) });
}
