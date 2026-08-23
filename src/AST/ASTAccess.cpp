#include "AST/ASTAccess.h"

#include "AST/ASTConstFold.h"
#include "AST/ASTConstness.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

#include <algorithm>

AST::AccessEffect AST::access_effect_of(const AST::VarDeclNode &decl)
{
    // `mv` is `t_take` said in the older spelling, so it folds in here rather than becoming a second
    // answer. its own enforcement - the "write mv at the call site" refusal in AST::OwnershipPass -
    // still reads the flag, because that is a rule about how the *call* is written and this is a
    // question about what the parameter does to storage
    if (decl.takes_ownership) {
        return AST::AccessEffect::t_take;
    }

    if (decl.access_effect != AST::AccessEffect::t_none) {
        return decl.access_effect;
    }

    // **a `const T&` is a read, and nothing else is inferred.**
    //
    // this is the whole of where the rule comes from without a keyword, and it is safe to infer for
    // exactly one reason: a read is the *non-exclusive* effect, so inferring one can never on its own
    // refuse anything. the exclusive half always has to be said - by a mutating method's receiver,
    // which is where a container's own invariant already lives, or by a word the author wrote.
    //
    // so a plain `T& $x` stays `t_none` rather than becoming `t_inout`: exclusivity inferred from a
    // parameter's type would silently refuse `swap(&$x, &$x)` and every program shaped like it, and
    // the promise nobody made is not one to hold them to.
    //
    // a *borrow* and not a by-value `const int32 $n`, which copies and reaches none of the caller's
    // storage at all
    if (decl.has_type() && AST::is_const_borrow(decl.type())) {
        return AST::AccessEffect::t_read;
    }

    return AST::AccessEffect::t_none;
}

AST::AccessEffect AST::declared_access_effect(const AST::VarDeclNode &decl)
{
    if (decl.takes_ownership) {
        return AST::AccessEffect::t_take;
    }

    return decl.access_effect;
}

AST::AccessEffect AST::access_effect_of(const AST::FunctionDeclNode &decl, size_t index)
{
    // a receiver's effect is what kind of member the function is, and nothing written on it - the
    // same shape AST::receiver_is_const takes, and for the same reason: `$this` is args[0] like any
    // other parameter, so a second carrier on the declaration would be a second answer
    //
    // has_receiver() rather than implicit_arg_count(), because a closure's args[0] is its environment
    // and not a receiver: nothing accesses a region through it. and rather than is_member(), because a
    // **static method** has an owner and no `$this` - its args[0] is an ordinary written parameter, so
    // answering the receiver rule for it would refuse legal calls against an effect nobody declared
    //
    // a **constructor** is absent from this switch on purpose, and its `t_constructor` arm would be
    // unreachable if it were here: its `$this` is a body-*local* that AST::declare_constructor_this
    // mints and marks `t_out`, so it has no receiver argument for this to answer about
    if (index == 0 && decl.has_receiver()) {
        switch (decl.member_kind) {
        case AST::MemberKind::t_destructor:
            // the receiver is going away and the body is what ends it
            return AST::AccessEffect::t_take;

        case AST::MemberKind::t_free:
        case AST::MemberKind::t_method:
        case AST::MemberKind::t_operator:
        case AST::MemberKind::t_constructor:
        // a **test** is as absent from this as a constructor is, and for a stronger reason: it takes no
        // arguments at all, so there is no index 0 for this to be asked about
        case AST::MemberKind::t_test:
        // a **static method** is absent for the reason the gate above gives - has_receiver() is false
        // for one, so this arm is unreachable. it is written out because the switch has no default,
        // which is what makes a kind added without a decision a compile error rather than a silent one
        case AST::MemberKind::t_static_method:
            return AST::receiver_is_const(decl) ? AST::AccessEffect::t_read : AST::AccessEffect::t_inout;
        }
    }

    if (index >= decl.args.size() || decl.args[index] == nullptr) {
        return AST::AccessEffect::t_none;
    }

    return AST::access_effect_of(*decl.args[index]);
}

bool AST::root_owns_its_storage(const AST::VarDeclNode &decl)
{
    if (!decl.has_type()) {
        return false;
    }

    const AST::ValueType &type = decl.type();

    // each of these is a name for storage that is somewhere else, so two of them can be one thing.
    // a struct or a primitive local *is* its storage, and two declarations of one are two of them
    return !type.is_pointer()
        && !type.is_class()
        && !type.is_weak()
        && !type.is_interface();
}

// one step of the walk: how far `expr` gets before it either reaches a declaration or gives up.
// projections come out innermost-first and are reversed by the caller
//
// `record` is what makes the root-only ask cheap without becoming a second walk: a caller that only
// needs the declaration pays for neither the projection vector, nor a string copy per field step, nor
// an AST::const_fold per indexed one. the arms it takes are the same ones either way, which is the
// point - a separate root walk would drift from this one arm by arm
static bool collect_projections(AST::ExprNode *expr, AST::AccessPath &into, bool record)
{
    while (expr != nullptr) {
        // an implicit cast reconciles a type and never changes which storage is meant
        expr = AST::strip_implicit_casts(expr);

        if (expr == nullptr) {
            return false;
        }

        switch (expr->get_node_type()) {
        case AST::NodeType::n_varref:
        {
            auto *var_ref = static_cast<AST::VarRefNode *>(expr);
            if (!var_ref->is_var()) {
                return false;
            }

            into.root = &var_ref->get_var().decl();
            return true;
        }

        // all three only re-address storage that is already named: `&E`, `E:$`, and the deref the
        // pointer adjuster wrapped a read in. none of them is a step of the path
        case AST::NodeType::n_expr_addrof:
            expr = static_cast<AST::AddrOfExprNode *>(expr)->operand;
            break;

        case AST::NodeType::n_expr_peel:
            expr = static_cast<AST::PointerValueNode *>(expr)->operand;
            break;

        case AST::NodeType::n_expr_deref:
            expr = static_cast<AST::DerefExprNode *>(expr)->operand;
            break;

        case AST::NodeType::n_member_access:
        {
            auto *member = static_cast<AST::MemberAccessNode *>(expr);

            if (record) {
                AST::Projection step;
                step.kind = AST::ProjectionKind::t_field;
                step.field = member->get_member_name().value();
                into.projections.push_back(std::move(step));
            }

            auto &base = member->get_base_node();
            if (!base.has() || !base.is_expression_node()) {
                return false;
            }

            expr = base.unsafe_ptr<AST::ExprNode>();
            break;
        }

        case AST::NodeType::n_expr_index:
        {
            auto *index = static_cast<AST::IndexExprNode *>(expr);

            // a raw pointer index - `$a->storage->data:$[3]`. the walk **continues through the
            // base**, so the region a path names includes the storage its own pointers reach, and
            // `t_indirect` is what marks that the step went through one
            //
            // asked of AST::IndexExprNode::indexed_base_type, the owner of "is this a pointer index",
            // and deliberately not of a null `element_call`: that is *three* states - a pointer index,
            // a base whose type is not settled yet, and one already reported - and reading the last two
            // as pointer indirection makes an unresolved bracket name a region it does not
            if (index->indexed_base_type().is_pointer()) {
                if (record) {
                    AST::Projection step;
                    step.kind = AST::ProjectionKind::t_indirect;
                    into.projections.push_back(std::move(step));
                }

                expr = index->base;
                break;
            }

            // an unsettled or already-refused bracket names nothing this can follow
            if (index->element_call == nullptr) {
                return false;
            }

            // `operator [](receiver, i...)`: the receiver is what the path continues through, and a
            // single index is the only arity a number can tell apart
            auto &args = index->element_call->arguments;
            if (args.empty()) {
                return false;
            }

            if (record) {
                AST::Projection step;
                step.kind = AST::ProjectionKind::t_dynamic;

                if (args.size() == 2) {
                    const AST::ConstFoldResult folded = AST::const_fold(args[1]);
                    if (folded.is_folded() && folded.type.is_integer_type()) {
                        step.kind = AST::ProjectionKind::t_element;
                        step.index = folded.bits;
                    }
                }

                into.projections.push_back(std::move(step));
            }

            expr = args[0];
            break;
        }

        default:
            return false;
        }
    }

    return false;
}

AST::VarDeclNode *AST::access_root_of(AST::ExprNode *expr)
{
    AST::AccessPath path;
    return collect_projections(expr, path, false) ? path.root : nullptr;
}

AST::AccessPath AST::access_path_of(AST::ExprNode *expr)
{
    AST::AccessPath path;

    if (!collect_projections(expr, path, true)) {
        // a partial walk names nothing, so it must not leave half a path behind for the overlap rule
        // to read as a real one
        path.root = nullptr;
        path.projections.clear();
        return path;
    }

    std::reverse(path.projections.begin(), path.projections.end());
    return path;
}

// did this path reach its storage through an address it read, rather than only through declarations?
static bool goes_through_a_pointer(const AST::AccessPath &path)
{
    for (const AST::Projection &step : path.projections) {
        if (step.kind == AST::ProjectionKind::t_indirect) {
            return true;
        }
    }

    return false;
}

AST::Overlap AST::path_overlap(const AST::AccessPath &a, const AST::AccessPath &b)
{
    if (!a.is_known() || !b.is_known()) {
        return AST::Overlap::t_unknown;
    }

    if (a.root != b.root) {
        // two names for storage that is elsewhere may be two names for one thing
        if (!AST::root_owns_its_storage(*a.root) || !AST::root_owns_its_storage(*b.root)) {
            return AST::Overlap::t_unknown;
        }

        // and neither may a path that went through a pointer be called disjoint from one under a
        // different root: two structs each holding one raw address are two roots naming one region,
        // which nothing here can see. distinct *declarations* are distinct storage; the things their
        // pointers reach are not declarations
        if (goes_through_a_pointer(a) || goes_through_a_pointer(b)) {
            return AST::Overlap::t_unknown;
        }

        return AST::Overlap::t_disjoint;
    }

    const size_t common = std::min(a.projections.size(), b.projections.size());

    for (size_t i = 0; i < common; i++) {
        const AST::Projection &pa = a.projections[i];
        const AST::Projection &pb = b.projections[i];

        if (pa.kind != pb.kind) {
            return AST::Overlap::t_unknown;
        }

        switch (pa.kind) {
        case AST::ProjectionKind::t_field:
            if (pa.field != pb.field) {
                return AST::Overlap::t_disjoint;
            }
            break;

        case AST::ProjectionKind::t_element:
            if (pa.index != pb.index) {
                return AST::Overlap::t_disjoint;
            }
            break;

        case AST::ProjectionKind::t_dynamic:
            // two indices nothing folded are neither the same slot nor different ones
            return AST::Overlap::t_unknown;

        case AST::ProjectionKind::t_indirect:
            // and two reads of one address are neither. the *prefix* case never reaches here, which
            // is the one that matters: `$src` against `$src.storage.data[?]` returns overlap below
            // without comparing a step at all
            return AST::Overlap::t_unknown;
        }
    }

    // equal, or one is a prefix of the other - and a prefix *contains* what follows it, so
    // `$a` overlaps `$a->items[3]`. that is the case the whole rule exists for
    return AST::Overlap::t_overlap;
}

bool AST::narrowing_promotes_raw_storage(const AST::ValueType &from, const AST::ValueType &to)
{
    // `T&($p:$)` - the explicit cast that turns a nullable address into a trusted borrow. the one
    // promotion that is a *value* conversion rather than an address being taken of a place, which is
    // why it has its own question and its own call site
    return from.is_pointer() && from.is_nullable() && to.is_pointer() && !to.is_nullable();
}

bool AST::function_pointer_promotes_raw_storage(const AST::ValueType &from, const AST::ValueType &to)
{
    // a C function pointer is a trusted typed callable, the way a T& is a trusted typed borrow.
    // reinterpreting a raw word as one - or extracting the word to store it - is the promise
    return (from.is_pointer() && to.is_c_function())
        || (from.is_c_function() && to.is_pointer());
}

bool AST::place_is_raw_derived(AST::ExprNode *expr)
{
    while (expr != nullptr) {
        expr = AST::strip_implicit_casts(expr);

        if (expr == nullptr) {
            return false;
        }

        switch (expr->get_node_type()) {
        case AST::NodeType::n_expr_addrof:
            expr = static_cast<AST::AddrOfExprNode *>(expr)->operand;
            break;

        case AST::NodeType::n_expr_peel:
            expr = static_cast<AST::PointerValueNode *>(expr)->operand;
            break;

        // **the one arm that answers yes.** a deref of a *nullable* pointer went through a raw
        // address; a deref of a borrow did not, because a borrow is already the trusted form and
        // whoever minted it made the promise
        case AST::NodeType::n_expr_deref:
        {
            auto *deref = static_cast<AST::DerefExprNode *>(expr);

            if (deref->operand != nullptr && deref->operand->result_type().is_nullable()) {
                return true;
            }

            expr = deref->operand;
            break;
        }

        case AST::NodeType::n_expr_index:
        {
            auto *index = static_cast<AST::IndexExprNode *>(expr);

            // `$p:$[3]` - offsetting a raw address. asked of the owner of "is this a pointer index"
            // rather than of a null `element_call`, which also means "not settled yet" and "already
            // refused": reading either of those as raw offsetting reports a promotion on top of the
            // diagnostic the program already earned
            return index->indexed_base_type().is_pointer();
        }

        case AST::NodeType::n_member_access:
        {
            auto &base = static_cast<AST::MemberAccessNode *>(expr)->get_base_node();

            if (!base.has() || !base.is_expression_node()) {
                return false;
            }

            expr = base.unsafe_ptr<AST::ExprNode>();
            break;
        }

        default:
            return false;
        }
    }

    return false;
}

bool AST::borrow_promotes_raw_storage(const AST::ValueType &to, AST::ExprNode *operand)
{
    // only a *borrow* destination promotes. a `ptr<T>` stays raw however it was computed, which is
    // the whole reason pointer casting is not the boundary
    if (!to.is_pointer() || to.is_nullable() || operand == nullptr) {
        return false;
    }

    // **the *place*, never the operand's type.** `&$p` where `$p` is a `ptr<T>` local is the address
    // of an ordinary slot - the declaration is the storage, and nothing raw was involved. reading the
    // operand's type instead made every `ptr<ptr<T>>` in the language a promotion.
    //
    // this covers `&$p:$[0]`, the borrow a call argument gets, a receiver's auto-borrow and a
    // `return &...`, which all reach here as the place the borrow is taken of. the explicit narrowing
    // `T&($p:$)` is the one form that is *not* a place, and AST::narrowing_promotes_raw_storage is
    // its question
    return AST::place_is_raw_derived(operand);
}

const char *AST::access_effect_spelling(AST::AccessEffect effect)
{
    switch (effect) {
    case AST::AccessEffect::t_none:
        return "";
    case AST::AccessEffect::t_read:
        return "read";
    case AST::AccessEffect::t_inout:
        return "inout";
    case AST::AccessEffect::t_out:
        return "out";
    case AST::AccessEffect::t_take:
        return "mv";
    }

    return "";
}
