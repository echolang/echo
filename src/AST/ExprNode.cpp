#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarRefNode.h"
#include <map>

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

        // offsetting an address stays an address, scaled by the pointee's size
        if ((op == Token::Type::t_op_add || op == Token::Type::t_op_sub)
            && raw_left.is_pointer() && !raw_right.is_pointer()) {
            return raw_left;
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
    if (op_node != nullptr && op_node->op->is_identity_comparison()
        && (raw_left.is_class() || raw_right.is_class())) {
        return AST::ValueType(AST::ValueTypePrimitive::t_bool);
    }

    // operands are read in value position, so a pointer contributes its pointee: `$ref + 1`
    // adds to the int the reference points at, not to the address
    auto left = value_type_of(raw_left);
    auto right = value_type_of(raw_right);

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
    // `&$x` yields the non-nullable borrow `T&`, which widens implicitly to `ptr<T>`. that one
    // rule is what makes both `int32& $r = &$var;` and `ptr<int32> $p = &$var;` legal without a
    // cast (book/concept/pointers_and_refs_v2.md, "Two pointer types")
    //
    // built over the operand's own result type, with no peeling: taking the address of a
    // pointer variable must yield a pointer to that variable's slot
    return ValueType::make_pointer(operand->result_type(), false);
}

const std::string AST::AddrOfExprNode::node_description()
{
    return "addrof<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
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

AST::ValueType AST::IndexExprNode::result_type() const
{
    // indexing a ptr<T> yields a T, the element itself
    return value_type_of(base->result_type());
}

const std::string AST::IndexExprNode::node_description()
{
    return "index<" + result_type().get_type_desciption() + ">(" + base->node_description() + "[" + index->node_description() + "])";
}

AST::ValueType AST::DerefExprNode::result_type() const
{
    return value_type_of(operand->result_type());
}

const std::string AST::DerefExprNode::node_description()
{
    return "deref<" + result_type().get_type_desciption() + ">(" + operand->node_description() + ")";
}