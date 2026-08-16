#ifndef ASTPLACEEXPR_H
#define ASTPLACEEXPR_H

#pragma once

#include "AST/ASTNodeTypes.h"
#include "AST/ASTSourceToken.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"

#include <optional>

namespace AST
{
    class VarDeclNode;

    // **what storage does this expression have?** the one taxonomy behind every question the compiler
    // asks about an expression's address
    //
    // total by construction: the switch below names only the two answers that are *not* the common case,
    // so a node kind added later lands in `t_materializable` - the useful default - instead of silently
    // answering "no" the way an allow-list makes it (the same trap CLAUDE.md lists for
    // `NodeReference::is_expression_node()`)
    enum class StorageClass
    {
        // it has an address already, so `&E`, `E:$` and assigning to E are all meaningful
        t_place,

        // no address, but one can be minted: the value is bound to a temporary and *that* is addressed.
        // which is what a method receiver needs, a method taking the address of the value it is called
        // on, and what lets a literal answer a borrow parameter
        t_materializable,

        // no address, and it must not be given one. each of these is load-bearing, and each for its
        // own reason - see the switch
        t_addressless,
    };

    // **asked of the tag, because the tag is the whole of the rule.** the node-taking overload below is
    // what every caller uses; this one exists because the classification can then be stated and tested
    // for kinds no small program produces - a closure environment's allocation, a retain the ownership
    // pass inserts - without having to build one of each
    inline StorageClass storage_of(NodeType kind)
    {
        switch (kind) {
            case NodeType::n_varref:
            case NodeType::n_member_access:
            case NodeType::n_expr_deref:
            case NodeType::n_expr_index:
                return StorageClass::t_place;

            // **a `?->` chain's unwrapped base is a place**, and it has to be: everything after the `?->`
            // is an ordinary member chain, and a method call in there needs a receiver with an address.
            // the chain materialises the unwrapped value into a slot before running the continuation, so
            // there genuinely is one - see ExprCodegen::gen_optional_chain
            //
            // it owns nothing, exactly like `$this`: the slot borrows the value the chain already holds,
            // so nothing here is retained and nothing is dropped
            case NodeType::n_expr_chain_base:
            // `Session::$count` names storage the *type* owns - one global per (type, property).
            // a place like any other from here on: `&` takes its address, an assignment writes
            // through it, a borrow argument binds it. what is different is only where the address
            // comes from, which is LValueCodegen's arm and nothing else's
            case NodeType::n_expr_static_property:
                return StorageClass::t_place;

            // an array literal fills storage, so it has to *name* it - AST::OperatorRewriter expands it
            // into a declaration and one append per element, which needs a place to append into rather
            // than a slot to copy a value in to
            case NodeType::n_expr_array_literal:
            // NullNode::bound_type is set by the destination, so there is nothing here to size a slot
            // from until somebody else has spoken
            case NodeType::n_null:
            // already values that *mean* an address; giving one storage hands out a ptr<ptr<T>>
            case NodeType::n_expr_addrof:
            case NodeType::n_expr_peel:
            // a transient marker saying a *place* is being handed over. the storage question is about
            // its operand, and a `mv` that survives to codegen is a compiler defect either way
            case NodeType::n_expr_move:
            // likewise transient: a constant reference is replaced by a clone of the constant's
            // initializer before anything asks about storage, and whatever *that* is answers for it. one
            // that survives is a compiler defect, so the honest answer here is "not addressable"
            case NodeType::n_expr_const_ref:
            // transient for the same reason and answered the same way: a `const(...)` is replaced by the
            // literal it folded to before anything asks about storage, and *that* literal answers. the
            // question would be incoherent anyway - the address of a value the compiler worked out is not
            // a thing the program can be given
            case NodeType::n_expr_const:
            // a function's address is already a value. giving it a slot would hand
            // MaterializationScope storage it does not need
            case NodeType::n_expr_function_ref:
            // already carries its own request outward through the pending queue
            case NodeType::n_expr_temp_bind:
                return StorageClass::t_addressless;

            // a call, an indirect call, every literal, an arithmetic result, a cast, `strong($w)`,
            // `??`, `?->`, a closure, an `instanceof` - all the same thing, a value the program
            // computed and did not name.
            //
            // **an interpolated string literal is here on purpose**, unlike the two transient nodes
            // above: it stands for a concatenation, so it is a computed value and can be *given*
            // storage. answering addressless instead was silently wrong in one direction - a
            // `const string&` parameter stopped ranking as a temporary borrow, so `println("{$x}")`
            // was refused as an impossible conversion rather than bound to a slot
            default:
                return StorageClass::t_materializable;
        }
    }

    inline StorageClass storage_of(const ExprNode &expr)
    {
        return storage_of(expr.get_node_type());
    }

    // **is this a `match` every arm of which named storage?** one of the two shapes whose *value* is an
    // address without their being a place - the other is a call returning `T&`, which stays inside
    // AST::read_reaches_storage below because deciding it needs a `result_type` this signature does not
    // take. false for everything that merely holds a pointer, which is a *place* and is covered there too.
    //
    // the two shapes are one situation: there is no slot holding the address, the value is the address, and
    // reading through it is what the expression means. a `match` earns it the way a call does -
    // `E::one($v) => $v` hands back a borrow into the subject, and the phi joining the arms is of those
    // addresses. it is deliberately **not** AST::storage_of: a match has no address of its own, so
    // `&match (...) { ... }` is refused exactly as `&$o->get()` is, and for the same reason
    bool value_is_an_address(const ExprNode &expr);

    // true when the expression denotes storage: it has an address
    //
    // shared deliberately. four places have to agree on this question - the parser rejecting
    // `&($a + $b)`, the adjustment pass deciding value versus place position, the type checker
    // locating a diagnostic, and the lvalue codegen's dispatch. when each kept its own switch
    // they drifted, which is how member reads and member writes ended up disagreeing
    inline bool is_place_expression(const ExprNode &expr)
    {
        return storage_of(expr) == StorageClass::t_place;
    }

    // **does a read of this expression reach storage, rather than a value it computed?**
    //
    // every place does, by definition. the one shape that names no storage and still hands one back is a
    // **call returning a borrow**: `$a->at(0)`'s value *is* the address of the container's element, so a
    // value-position read of it reads *through* it and owes a copy of what it found, exactly as a place's
    // read does. four mirrors of that one answer - the deref AST::PointerAdjuster writes, the type
    // value_result_type yields below, AST::argument_fit's t_read_through rank, and the copy
    // AST::OwnershipPass owes. they have to agree, or a call returning a borrow is copied as a
    // handle with no retain
    //
    // a *different* question from AST::storage_of, which stays a deny-list about the address an
    // expression **has**: a call has none, so `&$o->get()` is still refused, a call is still not an
    // assignment target, and a call result can still be *given* storage as a temporary
    //
    // **a `ptr<T>` a call returned is not one.** reading through an address that may be absent is
    // something the program has to say - `:$`, an explicit cast, a `guard` - which is the line
    // AST::argument_fit's t_read_through rank, LValue::provenance and AST::access_path_of already draw
    // for this very expression. a `ptr<T>` *place* is still read through, because that read is of the
    // slot and the slot is certainly there
    //
    // the place test comes first because it is the cheap one and what almost everything answers, and the
    // node test sits ahead of result_type() so a literal, a binary or a cast never pays for the
    // derivation. an unsettled call answers `void`, so this is false until the call settles - which is
    // what makes it safe to ask from inside the monomorphizer's fixpoint
    inline bool read_reaches_storage(const ExprNode &expr, const ValueType &result_type)
    {
        if (is_place_expression(expr)) {
            return true;
        }

        // a `match` whose arms all named storage, which is the second shape whose value is an address. it
        // needs no nullability test the way a call does: a payload borrow is minted by AST::MatchResolution
        // and is non-nullable by construction, so there is no `ptr<T>` spelling of it to exclude
        if (value_is_an_address(expr)) {
            return true;
        }

        if (!is_call_expression(expr)) {
            return false;
        }

        return result_type.is_pointer() && !result_type.is_nullable();
    }

    inline bool read_reaches_storage(const ExprNode &expr)
    {
        return read_reaches_storage(expr, expr.result_type());
    }

    // **the expression the author wrote, under whatever the compiler wrapped around it.** an implicit
    // cast is inserted to reconcile a type, never to change a value the program can observe, so every
    // question about *what this expression is* has to be asked underneath one. an **explicit** cast
    // stops the walk: the user wrote that one, and `(int32?)null` is a null they meant to give a type to
    //
    // one owner because it had already been written three times - in AST::written_null_of, in the type
    // checker and inside AST::literal_string_value - and a fourth was about to be added below
    inline ExprNode *strip_implicit_casts(ExprNode *expr)
    {
        while (expr != nullptr && expr->get_node_type() == NodeType::n_type_cast) {
            auto *cast = static_cast<TypeCastNode *>(expr);

            if (!cast->is_implcit) {
                break;
            }

            expr = cast->expr;
        }

        return expr;
    }

    inline const ExprNode *strip_implicit_casts(const ExprNode *expr)
    {
        return strip_implicit_casts(const_cast<ExprNode *>(expr));
    }

    // **is there a place underneath the implicit casts, and which cast sits directly over it?**
    //
    // `storage_of` answers on the tag alone, deliberately, so a cast is `t_materializable` - it is a
    // value the program computed. That is right for a cast the *user* wrote and wrong for one
    // AST::CallResolver inserted to reconcile an argument with a parameter, which computes nothing:
    // what is under it is still the caller's storage, still owned by whoever named it. reading such a
    // cast as "not a place" is how a borrow read into a by-value parameter came to be handed over with
    // no copy and no retain
    //
    // hands back the **innermost** cast rather than the place itself, because the caller's job is to
    // rewrite the edge *under* it: a retain or a copy has to be typed as the value, not as whatever the
    // cast widened it to
    inline TypeCastNode *place_under_implicit_cast(ExprNode &expr)
    {
        if (expr.get_node_type() != NodeType::n_type_cast) {
            return nullptr;
        }

        auto *cast = static_cast<TypeCastNode *>(&expr);

        if (!cast->is_implcit || cast->expr == nullptr) {
            return nullptr;
        }

        if (TypeCastNode *inner = place_under_implicit_cast(*cast->expr)) {
            return inner;
        }

        return read_reaches_storage(*cast->expr) ? cast : nullptr;
    }

    // **may this expression be *given* storage?** asked in the parser, which is where a receiver is
    // decided and no type is known yet, and in AST::argument_fit, which has the expression but not yet
    // a decision. AST::OwnershipPass asks the type question that goes with it
    // (borrow_operand_needs_storage below) and is what actually binds the temporary
    inline bool can_bind_temporary(const ExprNode &expr)
    {
        return storage_of(expr) == StorageClass::t_materializable;
    }

    // **is this storage the compiler is not accounting for?**
    //
    // A *different* question from every other one in this header, which are all about whether an
    // expression has an address. This one is about who owes that address's value an end.
    //
    // A local gets a scope-exit drop, a temporary gets one from the frame it was bound to, and a
    // property gets one from its owner's teardown - so every one of those is storage some pass already
    // walks. What is left is storage reached **through a pointer**, `$p:$` and `$p:$[$i]`, which nothing
    // walks, because a pointer is not an owner.
    //
    // That is exactly the storage a container manages itself, and therefore exactly the storage the two
    // unsafe seams in `mem::` are allowed to touch: `mem::take` empties one, `mem::init` fills one.
    //
    // Two readers, one rule. AST::TypeChecker refuses both the same way, so "which storage may I move
    // through" cannot come to two answers
    //
    // The index arm asks AST::IndexExprNode::indexed_base_type rather than looking at the base's node
    // kind, because that is the sole owner of "is this a pointer index". `$a[$i]` on an `array<T>`
    // reaches the element operator and is a place the array accounts for, while `$p:$[$i]` is raw
    // storage and is not
    inline bool is_unaccounted_storage(const ExprNode &expr)
    {
        if (expr.get_node_type() == NodeType::n_expr_deref) {
            return true;
        }

        if (expr.get_node_type() == NodeType::n_expr_index) {
            return static_cast<const IndexExprNode &>(expr).indexed_base_type().is_pointer();
        }

        return false;
    }

    // **is there anything here to mint storage for at all?** the half the two requesting arms - a member
    // access's base and an `&`'s operand - genuinely share, so `$o->get()->tag` and `$o->get()->size()`
    // cannot answer it differently.
    //
    // The place test comes first because it is the cheap one, and it is what almost every operand
    // answers. Every compiler-inserted borrow - a receiver, a drop, a CallResolver coercion - is an
    // address of a place, and a member base is a place in all but the shape this exists for. Deriving
    // the type ahead of it would re-walk a whole `->` chain per link, once per fixpoint round, and throw
    // the answer away
    //
    // a **pointer** operand needs nothing: a borrow-returning call already is the address
    //
    // **it asks the place half of storage_of and deliberately not the addressless half**, which is the
    // one place the two questions come apart. can_bind_temporary answers "may the compiler *ask* for a
    // slot for this shape", which an array literal and a bare `null` must refuse. this answers "does an
    // operand somebody has already decided to address have anything to size a slot from" - and
    // `n_expr_temp_bind` is exactly the case where those differ: nothing may request one be materialised
    // as an argument, because it carries its own request, yet an `&` over one a nested bind produced
    // does need a slot. the type test below is what excludes the rest: a null and an array literal both
    // answer unknown here
    //
    // answers *with the type it had to derive* rather than with a bool, so the two narrower questions
    // below share the one derivation. MemberAccessNode::result_type() recurses the whole `->` chain, so a
    // second call is quadratic in chain depth on a pass that runs once per fixpoint round
    inline std::optional<ValueType> storageless_operand_type(const ExprNode &operand)
    {
        if (is_place_expression(operand)) {
            return std::nullopt;
        }

        const ValueType type = operand.result_type();

        if (type.is_pointer()) {
            return std::nullopt;
        }

        return type;
    }

    // **does an `&`'s operand need storage minted for it?** everything that lacks storage and has a type
    // concrete enough to size a slot from - which is every value the language can
    // produce, a literal and an arithmetic result included, not only the struct and class a member base
    // has to be
    //
    // is_undetermined_type is the one predicate covering all three shapes that cannot be given a slot,
    // and each for its own reason: an unsettled call, whose result_type() is still void; an unknown; and
    // anything still mentioning a type parameter, which a template body must never allocate for before
    // the monomorphizer has substituted it. all three are left to the type checker's located error,
    // which is where a reason belongs
    //
    // **two passes must agree on this exactly**, which is why it lives here rather than beside either:
    // AST::OwnershipPass mints the slot, and AST::TypeChecker's guard rail reports the case where nothing
    // did. a guard rail spelling its own copy of the predicate it polices cannot catch the predicate
    // drifting, which is the one thing it is for
    inline bool borrow_operand_needs_storage(const ExprNode &operand)
    {
        const std::optional<ValueType> type = storageless_operand_type(operand);
        return type.has_value() && !is_undetermined_type(*type);
    }

    // **does a member access's base need storage minted for it?** a strictly narrower question than the
    // one above, and narrower on purpose: this arm does not ask whether the value *can* be stored but
    // whether there is a member to reach once it is. so it stays on has_property_layout, which leaves an
    // interface base - requirements, storing nothing a `->` can index - on the type checker's error
    // rather than silently binding a temporary nothing will read
    inline bool member_base_needs_storage(const ExprNode &operand)
    {
        const std::optional<ValueType> type = storageless_operand_type(operand);
        return type.has_value() && type->has_property_layout();
    }

    // true when the expression can stand on the left of an assignment. every place can, plus one
    // shape that is not a place: `E:$` names the pointer slot itself, and writing to it re-seats
    // the pointer (`$p:$ = &$b`). an address (`$p:$:$`, which collapses to `&$p`) has nothing to
    // write back into, so it is not one
    inline bool is_assignable_target(const ExprNode &expr)
    {
        return is_place_expression(expr) || expr.get_node_type() == NodeType::n_expr_peel;
    }

    // the variable an expression ultimately addresses, walking through everything that only
    // re-addresses existing storage rather than naming new storage: every place, plus `&E` and
    // `E:$`, which change what is being said about an address without changing whose it is
    // null when the expression names no variable at all
    //
    // lives next to the predicates above because it encodes the same taxonomy. a new place kind
    // that updates the predicate and not this walk fails silently, by simply not finding the
    // variable
    VarDeclNode *place_root_of(ExprNode *expr);

    // **where does a diagnostic about this expression point?** re-exported here, where its four callers
    // already look, but owned by AST::source_token_of ([ASTSourceToken.h]) - which answers the same
    // question over *every* node kind rather than over expressions only, because a DILocation is asked
    // about whatever a walk is standing on. Keeping a second copy here would answer it differently the
    // first time either grew an arm, which is the whole reason the arms moved

    // the type an expression yields when it is *read*, which is what an inferred declaration
    // and an assignment target both want
    //
    // reading something that holds a pointer auto-dereferences it once, so `$copy = $r` over an
    // `int32&` infers int32 and copies the value - and so does `$copy = $a->at(0)`, a call whose
    // value *is* an address. an expression that computed its value is already the value it means:
    // `&$x` yields an address, so `$ref = &$var` still infers a pointer
    //
    // read_reaches_storage above is the whole of which is which, and it being shared is what stops
    // this from disagreeing with the deref AST::PointerAdjuster writes over the same expression -
    // they were two spellings, and this one answered `int32&` for a call while a declared
    // `operator +` over that operand was never minted at all
    //
    // the overload taking a `result_type()` the caller already has, for a site that wants both the
    // read type and the raw one - result_type() walks the expression's subtree, so a caller holding
    // the answer passes it rather than provoking it again. the place rule stays in one place
    inline ValueType value_result_type(const ExprNode &expr, const ValueType &result_type)
    {
        return read_reaches_storage(expr, result_type) ? value_type_of(result_type) : result_type;
    }

    inline ValueType value_result_type(const ExprNode &expr)
    {
        return value_result_type(expr, expr.result_type());
    }

    // **the** type an inferred declaration takes from its initializer: the value the initializer
    // reads as, with the `const` the declaration was written with put back on top - the inferred type
    // is the single source of truth, const included, so there is no node-level flag to disagree with
    //
    // two askers at two moments, which is the only difference between them: the parser, where the
    // declaration is written, and the monomorphizer's re-derivation sweep, for a declaration whose
    // initializer was a call that could not be settled yet. a second spelling that dropped either
    // half would make `const $x = f();` silently lose its `const` for the initializers the
    // fixpoint has to finish
    //
    // the overload below is the second half on its own, for the **third** moment: an array literal's
    // `result_type()` is unknown by construction, so a declaration initialized by one is typed from
    // its *elements* by AST::array_literal_type_for, and only the `const` half applies there
    inline ValueType infer_declaration_type(const ValueType &inferred, bool is_const)
    {
        // a pointer to unknown/void is still no information: `&$a[0]` is `void&` until the
        // bracket becomes an `operator []` call. collapsing it here - the one owner of what
        // an inferred declaration takes from its initializer - is what keeps the stale-variable
        // sweep asking, without teaching that sweep to peel. a `T&` stays `T&`: a type
        // parameter is information, and collapsing it would erase a generic's borrow
        if (inferred.is_pointer()) {
            const ValueType inner = value_type_of(inferred);
            if (inner.is_unknown() || inner.is_void()) {
                return infer_declaration_type(ValueType::make_unknown(), is_const);
            }
        }

        return is_const ? ValueType::make_const(inferred) : inferred;
    }

    inline ValueType infer_declaration_type(const ExprNode &init_expr, bool is_const)
    {
        return infer_declaration_type(value_result_type(init_expr), is_const);
    }
};

#endif
