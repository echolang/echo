#include "AST/ASTPointerAdjuster.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTRegion.h"
#include "AST/ASTNullability.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTVariadic.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ConstExprNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NullNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/TemporaryBindExprNode.h"

namespace AST
{

PointerAdjuster::PointerAdjuster(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef PointerAdjuster::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, token.make_slice()};
}

void PointerAdjuster::run()
{
    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();
        accept_semantic_roots(*module_ptr, *this, _current_file);
    }
}

ExprNode *PointerAdjuster::strip_peel(ExprNode *expr)
{
    if (expr == nullptr || expr->get_node_type() != NodeType::n_expr_peel) {
        return expr;
    }

    auto *peel = static_cast<PointerValueNode *>(expr);
    ExprNode *operand = peel->operand;

    // `:$` is only meaningful on something transparent to strip. reported here rather than in
    // the parser because a type parameter's pointer-ness is not known until monomorphization
    if (!operand->result_type().is_pointer()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(peel->token_peel),
            "':$' expects a pointer, got '" + operand->result_type().get_type_desciption() + "'");
    }

    // the marker's whole job was to stop as_value() inserting a deref here. with it gone the
    // operand stands as its own place, whose value *is* the pointer
    return operand;
}

ExprNode *PointerAdjuster::rewrite_value_edge(ExprNode *expr)
{
    return as_value(expr);
}

ExprNode *PointerAdjuster::rewrite_place_edge(ExprNode *expr)
{
    return adjust_place(expr);
}

ExprNode *PointerAdjuster::as_value(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    // a peeled expression is already the value it means - the pointer itself - so it keeps its
    // place and gets no deref. this is the entire implementation of `:$`
    if (expr->get_node_type() == NodeType::n_expr_peel) {
        expr->accept(*this);
        return strip_peel(expr);
    }

    expr->accept(*this);

    // a place holding a pointer needs the deref - and so does a **call returning a borrow**, whose
    // value *is* an address into somebody else's storage. AST::read_reaches_storage is the one answer
    // to which reads go through, and sharing it is what stops this from disagreeing with the type
    // AST::value_result_type yields for the same expression.
    //
    // an AddrOfExprNode is a pointer too, but it is already the value it means - `&$x` yields an
    // address, it does not read through one - and a `ptr<T>` a *call* returned is deliberately not one
    // either: reading through an address that may be absent is something the program has to say
    const ValueType type = expr->result_type();

    if (!type.is_pointer() || !read_reaches_storage(*expr, type)) {
        return expr;
    }

    return &_current_module->nodes.emplace_back<DerefExprNode>(expr);
}

ExprNode *PointerAdjuster::as_value_for(ExprNode *expr, const ValueType &wanted)
{
    // a pointer-shaped destination wants the address, not the thing at it. that is the one
    // rule separating a declaration that *binds* (`ptr<int32> $p = &$a;`) from an assignment
    // that writes *through* (`$p = 20;`), and it is the same rule either way.
    //
    // "the address" means the one that actually fits, though. an expression already a level too
    // deep for the destination - a ptr<ptr<int32>> handed to a ptr<int32> slot, which is what
    // `return $v` is inside `first<T>(ptr<T> $v) : T` once T is itself a pointer - still owes
    // one read. binding it unchanged produced a type the destination could not hold
    if (wanted.is_pointer() && expr != nullptr
        && is_implicitly_convertible(expr->result_type(), wanted)) {
        return adjust_place(expr);
    }

    return as_value(expr);
}

template <typename WantedAt>
void PointerAdjuster::adjust_call_arguments(std::vector<ExprNode *> &arguments, WantedAt wanted_at)
{
    for (size_t i = 0; i < arguments.size(); i++) {
        auto *&arg = arguments[i];

        // an argument already wrapped in an address-of was borrowed deliberately,
        // by the coercion pass or by the user writing `&$x`; leave it as the address
        if (arg != nullptr && arg->get_node_type() == NodeType::n_expr_addrof) {
            auto *addr = static_cast<AddrOfExprNode *>(arg);
            addr->operand = adjust_place(addr->operand);
            continue;
        }

        // a pack is the one argument list with no parameter opposite it, so each element is its
        // own destination. as_value_for still owns how far that destination is read: a `ptr<T>`
        // element keeps its address, anything else reads through
        if (ArrayLiteralExprNode *pack = variadic_pack_of(arg)) {
            for (auto *&element : pack->elements) {
                if (element != nullptr) {
                    element = as_value_for(element, element->result_type());
                }
            }

            continue;
        }

        // otherwise the parameter decides how far to read: a pointer parameter takes
        // the address as it is, anything else reads through. that is also where the
        // generic decay lands - an inferred `T` is not a pointer, so a pointer
        // argument is read to its pointee
        arg = as_value_for(arg, wanted_at(i));
    }
}

ExprNode *PointerAdjuster::adjust_place(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    expr->accept(*this);
    return strip_peel(expr);
}

void PointerAdjuster::bind_null_operand(ExprNode *maybe_null, ExprNode *other)
{
    if (maybe_null == nullptr || other == nullptr) {
        return;
    }

    // **the cheap half of the question first.** only a written null has anything to bind, and asking
    // AST::written_null_of is a tag compare - where `other->result_type()` below walks the other operand's
    // whole subtree, which for a nested binary or a member chain is not free. this is called for both
    // operands of every binary node in the program, and almost none of them is a null
    //
    // one walk, and it owns "is this a null at all" - the raw `n_null` tag is not the question, because an
    // implicit cast the parser or the monomorphizer wrapped around it hides that tag
    NullNode *null_node = written_null_of(maybe_null);

    if (null_node == nullptr || null_node->is_bound()) {
        return;
    }

    // the shared rule: does the other side admit absence? that covers a `ptr<T>`, a `weak<T>` and any `T?`
    // - including the wrapped shapes, whose `== null` is a tag test rather than an address comparison and
    // which therefore need the bound type to know their shape
    //
    // a weak is the *one* thing this admits that the other askers would refuse outright. `$w == null`
    // answers whether the reference was ever taken, not whether the object is still alive - that question
    // is `strong($w)`, because only reading the count can answer it
    //
    // **and a comparison-only widening on top of it**: a non-nullable class handle. it is an address, so
    // `$obj == null` lowers to an icmp over two handles and has always been accepted - even though the
    // answer is now statically known, because a `Foo` that is not a `Foo?` is never absent. narrowing that
    // to a diagnostic is a semantic decision of its own and is deliberately not made here; it is spelled at
    // this call site rather than inside destination_admits_null so the other askers cannot inherit it
    const ValueType other_type = other->result_type();

    if (destination_admits_null(other_type) || other_type.is_class()) {
        null_node->bound_type = other_type;
    }
}

// ---- the arms that are not the base's default -------------------------------------------------
//
// everything absent from this list is the base's descent plus as_value(), which is the overwhelmingly
// common answer and the safe one. that is the whole trade: a forgotten arm here reads a value where a
// place was wanted and AST::TypeChecker sees the mismatch, where a forgotten arm in the old switch
// meant an unvisited subtree and codegen reading the wrong number of levels with nothing to say

void PointerAdjuster::visitFunctionDecl(FunctionDeclNode &node)
{
    // a template's body is only meaningful once cloned into a concrete instance
    if (node.is_generic()) {
        return;
    }

    FunctionDeclNode *prev = _current_function;
    _current_function = &node;
    RecursiveVisitor::visitFunctionDecl(node);
    _current_function = prev;
}

void PointerAdjuster::visitVarDecl(VarDeclNode &node)
{
    // a declaration *binds*: `ptr<int32> $p = &$a;` stores the address, so the initializer keeps its
    // pointer when the declared type is a pointer too
    node.init_expr = as_value_for(
        node.init_expr, node.has_type() ? node.type() : ValueType::make_unknown());
}

void PointerAdjuster::visit_assign(AssignNode &node)
{
    // the bind first: its initializer is `&$rows[$at]`, and adjust_place has to rewrite that index
    // the way it already rewrites `T& $r = &$a[0]`. the reseated target below is a read of the bind,
    // so its type is already on the declaration
    statement_edge(node.target_bind);

    // the target goes through as_value like any other read: a plain pointer target gains the deref
    // that write-through means, while `$p:$` keeps the slot it names - that is the whole difference
    // between writing through a pointer and re-seating it
    node.target = as_value(node.target);

    // the value is then read to fit whatever storage the target turned out to name. for
    // `ptr<ptr<int>> $out`, `$out = $target` writes the caller's *pointer*, so the value keeps its
    // address instead of being read through.
    //
    // **order-dependent**: the destination is read off the target *after* the target was rewritten,
    // which is one of the three reasons a shared walker cannot precompute a destination
    node.value_expr = as_value_for(
        node.value_expr,
        node.target != nullptr ? node.target->result_type() : ValueType::make_unknown());

    // and the old value's teardown, which is a scope of ordinary destructor calls hanging off this
    // statement rather than sitting in the enclosing one's children. not cosmetic: a drop's receiver
    // is `AddrOf(place)`, whose operand is a place edge, so a member-path place with a pointer base
    // would otherwise silently lose its deref
    statement_edge(node.teardown_old);
}

void PointerAdjuster::visitFunctionCallExpr(FunctionCallExprNode &node)
{
    adjust_call_arguments(node.arguments, [&node](size_t i) {
        if (node.decl != nullptr && i < node.decl->args.size() && node.decl->args[i]->has_type()) {
            return node.decl->args[i]->type();
        }

        return ValueType::make_unknown();
    });
}

void PointerAdjuster::visit_indirect_call_expr(IndirectCallExprNode &node)
{
    // the callee is wanted as a *value*: `$p()` over a `ptr<function<...>>` has to read through to the
    // callable before its fn slot can be extracted. the parameter types come off the signature rather
    // than off a declaration, which is the whole difference from a direct call
    const ValueType callee_type = node.callee_type();
    node.callee = as_value_for(node.callee, callee_type);

    if (!callee_type.has_signature()) {
        return;
    }

    const auto &signature = callee_type.signature();

    adjust_call_arguments(node.arguments, [&signature](size_t i) {
        return i < signature.parameter_types.size()
            ? signature.parameter_types[i]
            : ValueType::make_unknown();
    });
}

void PointerAdjuster::visit_closure_expr(ClosureExprNode &node)
{
    // each captured place is read in the enclosing frame, as a value of the property it fills
    for (size_t i = 0; i < node.captured_values.size(); i++) {
        const ValueType wanted = node.environment_type != nullptr
            ? node.environment_type->get_property_type(i)
            : ValueType::make_unknown();

        node.captured_values[i] = as_value_for(node.captured_values[i], wanted);
    }
}

void PointerAdjuster::visitReturn(ReturnNode &node)
{
    // a return fits its value to the declared return type, exactly as a declaration and an assignment
    // do. a `T&` return hands back the address, so reading through it here would return the pointee
    // where the signature promised the pointer - which llvm's verifier rejects outright ("return type
    // does not match operand")
    node.expr = as_value_for(
        node.expr,
        _current_function != nullptr ? _current_function->get_return_type() : ValueType::make_unknown());

    // the drops this return owes. they were skipped while this pass drove its own traversal, on the
    // argument that AST::needs_destruction says a pointer is never an owner so a drop's place cannot
    // reach through one. the assign arm above already contradicted that argument for exactly the same
    // shape of node, so the walk is uniform now and the two agree
    statement_edges(node.unwind);
}

void PointerAdjuster::visitBinaryExpr(BinaryExprNode &node)
{
    RecursiveVisitor::visitBinaryExpr(node);

    // null has no type of its own, so in a comparison it takes the other operand's. done here rather
    // than in the parser because the other side's pointer-ness is only settled once the derefs above
    // are in place
    bind_null_operand(node.lhs, node.rhs);
    bind_null_operand(node.rhs, node.lhs);
}

void PointerAdjuster::visit_release(ReleaseNode &node)
{
    // deliberately not adjusted at all. codegen reads the target's slot itself, because between the
    // declaration and the release an assignment may have re-seated the variable - so the release wants
    // the place, not a read of it, and not a subtree walk that could strip a peel out from under it. a
    // borrow is never a release target anyway: needs_destruction is false for a pointer
}

void PointerAdjuster::visit_const_ref(ConstRefExprNode &node)
{
    // a transient node AST::ConstantExpander was supposed to have erased - by replacing it with a clone of
    // the constant's initializer, or by replacing it with a void after refusing it. one reaching here would
    // have every deref inside whatever it stood for silently skipped, which is AST::ForeachNode's contract
    // and for the same reason
    throw std::runtime_error(
        "a constant reference survived the constant expander - it should have been replaced by the "
        "constant's initializer, or by a void after a refusal");
}

void PointerAdjuster::visit_const_decl(ConstDeclNode &node)
{
    // reachable only from a root, and a constant declaration is a child of no root by construction
    throw std::runtime_error(
        "a constant declaration was reached from a body - it is owned by the arena and belongs to no scope");
}

// **the enforcement half of AST::ConstFolding::finalize().** a marker a pass was supposed to erase is a
// compiler bug, and answering with something plausible hides it: one reaching here would have every deref
// inside both arms silently skipped, and codegen would read the wrong number of levels with no diagnostic.
// AST::PointerValueNode's contract, and AST::ForeachNode's below
void PointerAdjuster::visit_const_if(ConstIfNode &node)
{
    throw std::runtime_error(
        "a 'const if' survived the monomorphizer's fixpoint - its condition should have selected an arm, "
        "or the statement should have been discarded after a refusal");
}

void PointerAdjuster::visit_const_expr(ConstExprNode &node)
{
    throw std::runtime_error(
        "a 'const(...)' survived the monomorphizer's fixpoint - it should have become the literal it "
        "folded to, or been refused");
}

void PointerAdjuster::visit_string_interpolation(StringInterpolationExprNode &node)
{
    // a transient node AST::InterpolationLowering was supposed to have erased - by lowering it into
    // the concatenation it stands for, or by discarding it after a refusal. ForeachNode's contract,
    // and for its reason: every deref inside a hole would be silently skipped otherwise
    throw std::runtime_error(
        "a string interpolation survived the monomorphizer's fixpoint - it should have been lowered "
        "into a concatenation, or discarded after a refusal");
}

void PointerAdjuster::visit_foreach(ForeachNode &node)
{
    // a transient node AST::ForeachLowering was supposed to have erased - by lowering it, or by
    // discarding it after a refusal. one reaching here would have every deref inside its body
    // silently skipped, and codegen would read the wrong number of levels with no diagnostic.
    // AST::PointerValueNode's contract
    throw std::runtime_error(
        "a 'foreach' survived the monomorphizer's fixpoint - it should have been lowered into "
        "an iterator and a while, or discarded after a refusal");
}

};
