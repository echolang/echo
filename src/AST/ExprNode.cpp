#include "AST/ExprNode.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/TypeCastNode.h"
#include "AST/LiteralValueNode.h"
#include <map>

// **`!` answers bool whatever it was applied to, and `-` answers its operand.** the two are not one
// rule, which is why this is a switch and not the one-liner it used to be: `!` over a nullable is a
// presence test, so the operand's type is `T?` and the answer is `bool` - the same divergence
// BinaryExprNode::result_type records below for a comparison
AST::ValueType AST::UnaryExprNode::result_type() const
{
    if (token_operator.type() == Token::Type::t_exclamation) {
        return AST::ValueType(AST::ValueTypePrimitive::t_bool);
    }

    if (expr == nullptr) {
        return AST::ValueType::make_void();
    }

    // negation preserves the operand type
    return expr->result_type();
}

AST::ValueType AST::BinaryExprNode::result_type() const
{
    if (lhs == nullptr || rhs == nullptr) {
        return AST::ValueType::make_void();
    }

    auto raw_left = lhs->result_type();
    auto raw_right = rhs->result_type();

    // arithmetic and comparison reached through `:$` operate on the address itself. these arms
    // read the operands raw, before the value-position collapse below, because that is exactly
    // the distinction `:$` draws: `$a:$ == $b:$` asks "same object?", `$a == $b` "same value?"
    if (op_node != nullptr && (raw_left.is_pointer() || raw_right.is_pointer())) {
        const auto op = op_node->op->type;

        // offsetting an address stays an address, scaled by the pointee's size. **mutable**, for the
        // reason spelled out below: the const on the operand's own level said its slot could not be
        // re-seated, and this answer is a fresh value with no slot to re-seat. the pointee's const
        // rides along untouched, which is the half that is actually a promise about storage
        if ((op == Token::Type::t_op_add || op == Token::Type::t_op_sub)
            && raw_left.is_pointer() && !raw_right.is_pointer()) {
            return AST::ValueType::make_mutable(raw_left);
        }

        // the distance between two addresses, in elements
        if (op == Token::Type::t_op_sub && raw_left.is_pointer() && raw_right.is_pointer()) {
            return AST::ValueType(AST::ValueTypePrimitive::t_int64);
        }

        if (op_node->op->is_comparison()) {
            return AST::ValueType(AST::ValueTypePrimitive::t_bool);
        }
    }

    // a class handle is an address too, but it is not a t_pointer, so it needs saying separately
    // `$a == $b` asks whether two references name one object, and `$a == null` whether one names
    // anything - both bool. every other operator on a class is rejected by the type checker, so
    // falling through to the "same type on both sides" rule below would answer `Counter` for a
    // comparison and hand echo a class where it expected a bool
    //
    // and a nullable of any kind, for the same reason and with the same answer: `$x == null` asks whether
    // a value is there. that covers the wrapped shapes - `int32?`, a nullable struct - which have no
    // address at all, so without this arm they fell through to "same type on both sides" and an
    // `int32? == null` answered `int32?`
    if (op_node != nullptr && op_node->op->is_identity_comparison()
        && (raw_left.is_class() || raw_right.is_class()
            || raw_left.is_nullable() || raw_right.is_nullable()
            || raw_left.is_weak() || raw_right.is_weak())) {
        return AST::ValueType(AST::ValueTypePrimitive::t_bool);
    }

    // **a comparison is a bool, whatever it compared.** the two arms above answer this for a pointer,
    // a class, a nullable and a weak; every other operand pair used to fall through to the "same type on
    // both sides" rule at the bottom, so `$a == $b` over two `int32`s answered **`int32`**.
    //
    // which mostly hid: `echo $a == $b` prints 1 or 0 either way, and a `bool` destination accepts the
    // integer. what it did not survive is a comparison becoming an *operand*:
    // `if ($a == 0 || $b > 3)` handed `gen_binary_expr` two `int32`s for the `||`, which took its
    // integer arm, found no case for a logical operator and threw an internal compiler error - so
    // `&&` and `||` over two comparisons could not be written at all. `const if` was unaffected, because
    // AST::const_fold answers the two logical operators from the folded values rather than from a type
    //
    // asked of AST::Operator::is_comparison, which is the one owner of "which symbols are these" - the
    // six of them, and deliberately not `&&`/`||`, whose bool-ness comes from their operands being bools
    if (op_node != nullptr && op_node->op != nullptr && op_node->op->is_comparison()) {
        return AST::ValueType(AST::ValueTypePrimitive::t_bool);
    }

    // operands are read in value position, so a pointer contributes its pointee: `$ref + 1`
    // adds to the int the reference points at, not to the address
    //
    // **and mutable.** `const` is a promise about *storage*, and this expression produces a fresh
    // value that has none - `$a + 1` is no more const than `1` is. reading the operands raw made the
    // equality below distinguish `usize` from `const usize`, so every arithmetic reached through a
    // const receiver answered void: untyped for the type checker, and an unhandled operand kind by
    // the time codegen saw it. the collapse and this strip are the two halves of "read this operand
    // as a value"
    auto left = ValueType::make_mutable(value_type_of(raw_left));
    auto right = ValueType::make_mutable(value_type_of(raw_right));

    // **a shift is its left operand's type, whatever the count is written as.** every arm below this
    // one is about two operands meeting, and a count is not an operand - see
    // AST::binary_reconciles_operands. Without this arm `$byte << $wide_count` answered *void*, which
    // is the "not reconciled yet" signal, and OperatorRewriter::widen_binary_operands duly reconciled
    // it - so a `uint8` shifted by an `int64` became an `int64` shift and left the type it was held in
    if (op_node != nullptr && op_node->op != nullptr && !binary_reconciles_operands(op_node->op)) {
        return left;
    }

    // if both left and right have the same type then the result type is the same
    if (left == right) {
        return left;
    }

    return AST::ValueType::make_void();
}

AST::ValueType AST::FunctionCallExprNode::result_type() const
{
    if (decl == nullptr) {
        return AST::ValueType::make_void();
    }

    return decl->get_return_type();
}

const std::string AST::FunctionCallExprNode::decorated_func_name() const
{
    return decl ? decl->decorated_func_name() : token_function_name.value();
}

const std::string AST::FunctionCallExprNode::node_description()
{
    std::string desc = "call ";

    // the template's own name, even for a generic call: --print-ast dumps the tree as parsed,
    // before monomorphization, so there is no instance to name yet. --print-instances is where
    // the resolved instantiations belong
    desc += decl ? decl->namespaced_func_name() : token_function_name.value();

    desc += "(";

    for (auto arg : arguments) {
        desc += arg->node_description() + ", ";
    }

    if (arguments.size() > 0) {
        desc = desc.substr(0, desc.size() - 2);
    }

    desc += "): " + result_type().get_type_desciption();

    return desc;
}

AST::ValueType AST::AddrOfExprNode::result_type() const
{
    // a written `&` over a counted object is a **weak reference** to it, not the address of the slot
    // holding the handle. the slot address is what the uniform rule below would give, and it is
    // essentially never what a program means: a class handle is already an address, so `ptr<Foo>` is a
    // pointer to a pointer, while what `&$obj` is reaching for is a second name that does not own
    //
    // see the node's header for why this is one bit rather than a second node, and why it is answered
    // here rather than in the parser
    const ValueType operand_type = operand->result_type();

    if (denotes_weak_reference(operand_type)) {
        return ValueType::make_weak(operand_type);
    }

    // `&$x` yields the non-nullable borrow `T&`, which widens implicitly to `ptr<T>`. that one
    // rule is what makes both `int32& $r = &$var;` and `ptr<int32> $p = &$var;` legal without a
    // cast (book/concept/pointers_and_refs_v2.md, "Two pointer types")
    //
    // built over the operand's own result type, with no peeling: taking the address of a
    // pointer variable must yield a pointer to that variable's slot
    return ValueType::make_pointer(operand_type, false);
}

const std::string AST::AddrOfExprNode::node_description()
{
    // the two meanings read differently in a dump, because a `-ar` reader chasing a missing weak release
    // needs to see which one the tree holds without deducing it from the rendered type
    const std::string form = denotes_weak_reference() ? "weakref" : "addrof";

    return form + "<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
}

AST::ValueType AST::StrongExprNode::result_type() const
{
    const ValueType operand_type = operand->result_type();

    // not a weak: unknown rather than an assert, so TypeChecker can report it against the token this node
    // carries. `unknown` is read everywhere as "says nothing" rather than as a mismatch, which is also
    // what keeps a generic whose operand is still a bare `T` from being judged a round too early
    if (!operand_type.is_weak()) {
        return ValueType::make_unknown();
    }

    // the class it names, **nullable** - a dead object is a real answer, and it is the type that makes the
    // program acknowledge it rather than a convention. see book/concept/nullability.md
    return ValueType::make_nullable(operand_type.weak_target());
}

AST::ValueType AST::NullCoalesceExprNode::result_type() const
{
    const ValueType right = rhs->result_type();

    // a nullable right side leaves the whole expression nullable - `$a ?? $b` over two `int32?`s can still
    // be absent, and saying so is what lets it chain into another `??`
    if (right.is_nullable()) {
        return right;
    }

    // otherwise the left's payload, which is the point: the value is there on both paths, so the type
    // stops carrying a `?` and the result needs no further unwrapping
    return ValueType::make_non_nullable(lhs->result_type());
}

const std::string AST::NullCoalesceExprNode::node_description()
{
    return "coalesce<" + result_type().get_type_desciption() + ">("
        + lhs->node_description() + ", " + rhs->node_description() + ")";
}

AST::ValueType AST::OptionalChainExprNode::result_type() const
{
    if (continuation == nullptr) {
        return ValueType::make_unknown();
    }

    const ValueType reached = continuation->result_type();

    // a call that answers nothing has nothing to be absent. `$a?->save()` is a statement either way, and
    // wrapping void would invent a value for the statement to discard
    if (reached.is_void() || is_undetermined_type(reached)) {
        return reached;
    }

    // **not wrapped twice.** there is one `null` in the language, so a continuation that is already
    // nullable stays exactly as nullable - `$a?->maybeB()` is one `B?`, not an absence inside an absence
    return ValueType::make_nullable(reached);
}

const std::string AST::OptionalChainExprNode::node_description()
{
    return "optchain<" + result_type().get_type_desciption() + ">("
        + base->node_description() + ", "
        + (continuation != nullptr ? continuation->node_description() : "<none>") + ")";
}

const std::string AST::StrongExprNode::node_description()
{
    return "strong<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
}

AST::ValueType AST::PointerValueNode::result_type() const
{
    // unpeeled: `$p:$` is the ptr<int32> that `$p` would otherwise read through
    return operand->result_type();
}

const std::string AST::PointerValueNode::node_description()
{
    return "peel<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
}

AST::ValueType AST::MoveExprNode::result_type() const
{
    // a move changes who owns the value, not what it is
    return operand->result_type();
}

const std::string AST::MoveExprNode::node_description()
{
    return "move<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
}

const std::string AST::ClassAllocExprNode::node_description()
{
    return "alloc<" + class_type.get_type_desciption() + ">()";
}

AST::ValueType AST::RetainExprNode::result_type() const
{
    return operand->result_type();
}

AST::ValueType AST::ClosureExprNode::result_type() const
{
    if (decl == nullptr) {
        return ValueType::make_void();
    }

    return decl->callable_type();
}

AST::ValueType AST::IndirectCallExprNode::callee_type() const
{
    if (callee == nullptr) {
        return ValueType::make_void();
    }

    return value_type_of(callee->result_type());
}

AST::ValueType AST::IndirectCallExprNode::result_type() const
{
    // void rather than an assert when the callee is not callable: the type checker owns that
    // diagnostic, and result_type is asked while the tree is still half-resolved. the same contract a
    // FunctionCallExprNode with no decl answers under
    const ValueType type = callee_type();

    if (!type.is_callable()) {
        return ValueType::make_void();
    }

    return type.signature().return_type;
}

const std::string AST::ClosureExprNode::node_description()
{
    return "closure<" + result_type().get_type_desciption() + ">("
        + (decl != nullptr ? decl->decorated_func_name() : "<null>")
        + (captured_values.empty() ? ", no captures" : ", captures " + std::to_string(captured_values.size()))
        + ")";
}

const std::string AST::IndirectCallExprNode::node_description()
{
    std::string buffer = "call_indirect " + (callee != nullptr ? callee->node_description() : "<null>") + "(";

    for (size_t i = 0; i < arguments.size(); i++) {
        buffer += (i > 0 ? ", " : "") + arguments[i]->node_description();
    }

    return buffer + "): " + result_type().get_type_desciption();
}

const std::string AST::InstanceOfExprNode::node_description()
{
    return "instanceof<" + queried_type.get_type_desciption() + ">(" + operand->node_description() + ")";
}

const std::string AST::RetainExprNode::node_description()
{
    return "retain<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
}

AST::ValueType AST::IndexExprNode::indexed_base_type() const
{
    if (base == nullptr) {
        return ValueType::make_unknown();
    }

    ValueType type = base->result_type();

    while (type.is_pointer() && !type.is_nullable()) {
        type = type.pointee();
    }

    return type;
}

AST::ValueType AST::IndexExprNode::result_type() const
{
    // a container's element contract hands back a borrow of the element, so the element itself is
    // one level in from what the call yields - the same peel a pointer base gets below, asked of the
    // operator's return type instead of the base's
    if (element_call != nullptr) {
        return value_type_of(element_call->result_type());
    }

    const ValueType indexed = indexed_base_type();

    // **only a pointer answers for its own element.** anything else is a container whose contract
    // AST::OperatorRewriter has not attached yet, and peeling the base there would hand back the
    // *container* as though it were the element - a confidently wrong type that no later pass could
    // tell from a right one. unknown is the honest answer, and it is the one every pass already
    // knows how to wait on
    if (!indexed.is_pointer()) {
        return ValueType::make_unknown();
    }

    // indexing a ptr<T> yields a T, the element itself
    return value_type_of(indexed);
}

const std::string AST::IndexExprNode::node_description()
{
    // the rewritten form prints the call, because the call is what is there - `-ar` showing an
    // ordinary resolved `operator []` is the whole claim that a subscript is not a special case
    if (element_call != nullptr) {
        return "index<" + result_type().get_type_desciption() + ">("
            + element_call->node_description() + ")";
    }

    std::string arguments;
    for (auto *index : indices) {
        if (!arguments.empty()) {
            arguments += ", ";
        }
        arguments += index->node_description();
    }

    return "index<" + result_type().get_type_desciption() + ">("
        + (base != nullptr ? base->node_description() : "?") + "[" + arguments + "])";
}

const std::string AST::ArrayLiteralExprNode::node_description()
{
    std::string rendered;

    for (auto *element : elements) {
        if (!rendered.empty()) {
            rendered += ", ";
        }
        rendered += element->node_description();
    }

    return "arraylit([" + rendered + "])";
}

AST::ValueType AST::DerefExprNode::result_type() const
{
    return value_type_of(operand->result_type());
}

const std::string AST::DerefExprNode::node_description()
{
    return "deref<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
}
std::optional<std::string> AST::literal_string_value(const AST::ExprNode *expr)
{
    // through the implicit casts the resolver wraps an argument in to make it fit its parameter.
    // without this a nullability or const adjustment around an otherwise perfectly good literal
    // would read as "not a literal", and the message would silently lose its text
    expr = AST::strip_implicit_casts(expr);

    if (expr == nullptr || expr->get_node_type() != AST::NodeType::n_literal_string) {
        return std::nullopt;
    }

    return static_cast<const AST::LiteralStringExprNode *>(expr)->get_string_value();
}
