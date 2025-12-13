#include "AST/ASTConstness.h"

#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

bool AST::receiver_is_const(const AST::FunctionDeclNode &decl)
{
    // a free function has no receiver to be const, and a half-parsed member may not have pushed one
    // yet - both answer false rather than asserting, since every caller is asking about a declaration
    // that may still be settling
    if (!decl.is_member() || decl.args.empty()) {
        return false;
    }

    return is_const_borrow(decl.parameter_type(0));
}

bool AST::const_receiver_refused(const AST::FunctionDeclNode &callee, const ValueType &receiver)
{
    // only a method is refused this way. a free function's parameters carry their own const, and the
    // ordinary argument diagnostic already words that against the parameter it belongs to
    if (!callee.is_member() || callee.args.empty()) {
        return false;
    }

    // a destructor is reached only by drops AST::OwnershipPass synthesizes, which address a const
    // place deliberately - teardown is not one of the program's writes. saying nothing here is what
    // lets that stay a rule of the ownership pass rather than a hole in this one
    if (callee.is_destructor()) {
        return false;
    }

    return is_const_borrow(receiver) && !receiver_is_const(callee);
}

std::string AST::const_receiver_refusal(const AST::FunctionDeclNode &callee, const ValueType &receiver)
{
    if (!const_receiver_refused(callee, receiver)) {
        return {};
    }

    // the *value* the receiver borrows, so the message names `Box` rather than `const Box&` - the
    // reader already knows their value is const, what they need is which method refused it
    const std::string owner = ValueType::make_mutable(receiver.pointee()).get_type_desciption();

    return fmt::format(
        "cannot call '{}' on a const '{}' - the method is not declared const, so it may write. "
        "Mark it `const function {}(...)` if it only reads.",
        callee.signature_description(), owner, callee.func_name());
}


AST::ComplexType *AST::enclosing_type_of(const AST::FunctionDeclNode &decl)
{
    // a constructor is registered as a *free* declaration - `Foo(...)` builds a value, it is not
    // reached through a receiver - so `owner_type` is null on one and the type it belongs to is the
    // thing it returns. that is the body that most needs to reach a private property, so getting this
    // arm wrong would put every constructor outside its own type
    if (decl.is_constructor()) {
        AST::ValueType returned = decl.get_return_type();
        return returned.has_property_layout() ? returned.get_complex_type() : nullptr;
    }

    return decl.owner_type;
}

bool AST::can_reach_private_member(const AST::ComplexType *from, const AST::ComplexType *owner)
{
    if (from == nullptr || owner == nullptr) {
        return false;
    }

    // through the template on both sides: a method of `mem::buffer<int32>` and a property declared on
    // `mem::buffer<T>` are the same declaration seen from two instantiations, and comparing the
    // instances would make privacy a property of which one you happened to be looking at
    return from->template_or_self() == owner->template_or_self();
}
