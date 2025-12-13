#ifndef ASTCONSTNESS_H
#define ASTCONSTNESS_H

#pragma once

#include "AST/ASTValueType.h"

#include <string>

namespace AST
{
    class ComplexType;
    class FunctionDeclNode;

    // **a method's const-ness is the type of its receiver, and nothing else.**
    //
    // there is no flag on FunctionDeclNode and no sixth MemberKind, deliberately. `$this` is args[0]
    // like any other parameter, so writing `const Foo&` there is already enough for AST::argument_fit
    // to rank the call, AST::mangle_function_name to give the two spellings distinct symbols and
    // AST::match_function to prefer the exact receiver - none of which needed an arm. a second
    // carrier would be a second answer, and this codebase's recurring bug is exactly that
    //
    // so this is a *reader*, not a store: it asks the receiver what it already says
    bool receiver_is_const(const FunctionDeclNode &decl);

    // **the shape a const receiver has**, `ptr<const Foo>`, as one test rather than the two halves
    // spelled at each site. `const` is a per-level flag on ValueType, so `const ptr<Foo>` is a
    // different type entirely - a promise about the pointer slot, not about what it reaches - and
    // getting the level wrong is the mistake this exists to make unwritable
    inline bool is_const_borrow(const ValueType &type)
    {
        return type.is_pointer() && type.pointee().is_const();
    }

    // **const is a property of the path to storage, not of its declared type.**
    //
    // `->` reaching a property through a `const Foo&` yields a const property, however the property
    // was declared - otherwise a const receiver is decorative and `const Inner& $r = &$o->in;
    // $r->x = 5;` writes through a borrow that promised not to.
    //
    // one line, but it is *the* rule, so it has one home: AST::MemberAccessNode::result_type applies
    // it and AST::TypeChecker::check_const_target then reads the answer off result_type() the way it
    // already does for every other place. a second spelling anywhere else is how the two come to
    // disagree about which storage a write reaches
    //
    // asked of base_target_type()'s answer, which is what `->` addresses - so a `ptr<const Foo>` base
    // and a `const Foo&` one are one case, target_type_of having already peeled both
    inline ValueType member_type_through(const ValueType &base_target, const ValueType &member)
    {
        return base_target.is_const() ? ValueType::make_const(member) : member;
    }

    // **may `receiver` call `callee`?** the question on its own, with no wording built - the split
    // AST::conforms_to / AST::first_unmet_requirement takes, and for the same reason: AST::CallResolver
    // has to ask this while coercing (a cast it wrote over a refusal like this one is noise codegen has
    // no lowering for), and it is not the pass that owns diagnostics. asking it through the string form
    // and testing `.empty()` formats a full diagnostic for every member call in the program
    //
    // the receiver reaches here already addressed - the parser does that, not the coercion - so this
    // takes the borrow, `const Box&`, and not the value
    bool const_receiver_refused(const FunctionDeclNode &callee, const ValueType &receiver);

    // the same rule, worded - located and collected by AST::TypeChecker. empty when there is nothing to
    // refuse, so a caller that only wants the question asks the predicate above instead
    std::string const_receiver_refusal(const FunctionDeclNode &callee, const ValueType &receiver);

    // **the type a body is written inside**, or null at file scope.
    //
    // not `owner_type`, close as that reads: a *constructor* is registered as a free declaration and
    // carries a null owner, so asking that one would put every constructor outside its own type - and
    // a constructor is exactly the body that has to reach a private property. this reads the return
    // type for one and the owner for everything else, which is the same question asked of the two
    // shapes a member declaration has.
    //
    // two readers, and they are why this is one function: AST::TypeChecker asks it to decide whether a
    // site may reach a `private` property, and AST::ConstantExpander asks it for what `self::` denotes.
    // held apart they answered "inside the type" differently - one accepted an interface, the other did
    // not - so a body could be inside its type for one rule and outside it for the other
    ComplexType *enclosing_type_of(const FunctionDeclNode &decl);

    // **may a body written inside `from` reach a property declared private on `owner`?**
    //
    // one function so the rule has one home, and deliberately the whole rule: privacy is per *type*
    // and not per file or per namespace, so a nested type does not see its owner's privates and a
    // second declaration in the same namespace does not either. an instantiation is asked through
    // `declaration_type()`, so `mem::buffer<int32>`'s own method reaches what `mem::buffer<T>`
    // declared - without that, privacy would hold for a template and evaporate for every instance
    bool can_reach_private_member(const ComplexType *from, const ComplexType *owner);
};

#endif
