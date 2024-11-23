#include "AST/ASTTypeChecker.h"

#include "AST/ASTModule.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/AssignNode.h"
#include "AST/VarRefNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ReturnNode.h"
#include "AST/ASTPlaceExpr.h"

#include <fmt/core.h>

namespace AST
{

// the destinations that have no conversion to fall back on, and so have to be satisfied exactly:
// a pointer's conversions are directional (`T&` widens to `ptr<T>`, never the reverse), and a
// struct or class has none at all
//
// primitive-to-primitive is deliberately *not* in here - fitting an int32 literal into a float64
// slot is TypeLowering::coerce_value's job, which is why this is not simply
// `!is_implicitly_convertible`. the struct half is what catches `Foo $x = 42;`: the parser used to
// reject that while typing the literal, but a hint that cannot type a literal is now ignored there,
// and coerce_value passes a non-primitive destination straight through
static bool demands_exact_conversion(const ValueType &type)
{
    return type.is_pointer() || type.has_complex_type();
}

// names the storage an assignment target denotes, so a const diagnostic can say what the user
// wrote rather than only what its type is. empty for the shapes that have no name of their own
static std::string place_description(const ExprNode &expr)
{
    switch (expr.get_node_type()) {
        case NodeType::n_varref:
        {
            auto &ref = static_cast<const VarRefNode &>(expr);
            return ref.is_var() ? ref.get_var().decl().name_full() : "";
        }

        case NodeType::n_member_access:
            return static_cast<const MemberAccessNode &>(expr).get_member_name().value();

        default:
            return "";
    }
}

// numeric/primitive conversions are inserted by the parser/monomorphizer as casts, so only a
// fundamental kind mismatch (struct vs primitive, or two distinct struct identities) is a real
// argument error here. undeterminable types (void/unknown) are left to other diagnostics
static bool arg_assignable_to(const ValueType &arg, const ValueType &param)
{
    if (arg.get_kind() == ValueTypeKind::t_unknown || param.get_kind() == ValueTypeKind::t_unknown) {
        return true;
    }
    if (arg.is_void()) {
        return true;
    }

    // pointers match structurally on their pointee, with the borrow-widens-to-nullable rule
    // this arm is load bearing: a reference parameter used to reach the primitive arm below,
    // because `int32&` was an int32 carrying a flag rather than a kind of its own
    if (arg.is_pointer() || param.is_pointer()) {
        return is_implicitly_convertible(arg, param);
    }

    if (arg.is_primitive() && param.is_primitive()) {
        return true;
    }
    if (arg.has_complex_type() && param.has_complex_type()) {
        return arg.get_complex_type() == param.get_complex_type();
    }
    return false;
}

// looks through the implicit casts the parser and monomorphizer wrap around an argument, to the
// expression the user actually wrote. `null` is the case that needs it: the null-specific rules
// all test for the raw n_null tag, and a cast inserted to reconcile the argument with its
// parameter hides that tag behind an n_type_cast
static ExprNode *strip_implicit_casts(ExprNode *expr)
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

TypeChecker::TypeChecker(Bundle &bundle) :
    _bundle(bundle),
    _collector(bundle.collector)
{
}

CodeRef TypeChecker::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

void TypeChecker::run()
{
    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();
        for (auto &file : module_ptr->files()) {
            _current_file = &file;
            if (file.root) {
                file.root->accept(*this);
            }
        }
    }
}

void TypeChecker::visitFunctionDecl(FunctionDeclNode &node)
{
    // a generic template's body legitimately mentions its type parameters; it is only
    // meaningful once cloned into a concrete instance, which is checked separately
    if (node.is_generic()) {
        return;
    }

    FunctionDeclNode *prev = _current_function;
    _current_function = &node;
    RecursiveVisitor::visitFunctionDecl(node);
    _current_function = prev;
}

void TypeChecker::visitReturn(ReturnNode &node)
{
    // a return at file scope has no signature to answer to, and a synthesized return (the one
    // the struct parser builds for a constructor) has no token to report against
    if (_current_function != nullptr && node.expr != nullptr && node.token_return.has_value()) {
        const ValueType declared = _current_function->get_return_type();
        const ValueType actual = node.expr->result_type();

        check_destination_fits(Destination::t_return, declared, *node.expr, node.token_return.value());

        // the storage a local names is gone before the caller can read it, so handing back its
        // address is always wrong (book/concept/pointers_and_refs_v2.md, "Lifetimes")
        // a parameter is the caller's storage and outlives the call, so it is the legal case
        if (declared.is_pointer() && actual.is_pointer()) {
            VarDeclNode *root = place_root_of(node.expr);
            if (root != nullptr) {
                bool is_parameter = false;
                for (auto *arg : _current_function->args) {
                    if (arg == root) {
                        is_parameter = true;
                        break;
                    }
                }

                if (!is_parameter) {
                    _collector.collect_issue<Issue::GenericError>(
                        code_ref_for(node.token_return.value()),
                        fmt::format("cannot return the address of local '{}' - its storage ends with the call",
                            root->name_full()));
                }
            }
        }
    }

    RecursiveVisitor::visitReturn(node);
}

void TypeChecker::visit_type_decl(TypeDeclNode &node)
{
    // a generic struct template's property types legitimately mention its type parameters (the T
    // in `struct Box<T> { T $value; }`); it is only meaningful once instantiated with concrete
    // types. concrete/non-generic struct declarations are still checked
    if (node.is_generic()) {
        return;
    }
    RecursiveVisitor::visit_type_decl(node);
}

void TypeChecker::visitMemberAccess(MemberAccessNode &node)
{
    // the node answers this itself now. it used to be a second copy of the switch in
    // MemberAccessNode::result_type(), and the two drifted exactly as such pairs do: neither knew
    // an index base, so a typo'd member behind `$items:$[0]->` went unreported
    ValueType base_type = node.base_target_type();
    if (base_type.has_complex_type()) {
        ComplexType *complex = base_type.get_complex_type();
        const std::string member = node.get_member_name().value();
        if (complex && !complex->has_property(member)) {
            _collector.collect_issue<Issue::UnknownMember>(
                code_ref_for(node.get_member_name()),
                member,
                complex->name.value_or("<anonymous>"));
        }
    }

    // reaching a member needs an address to reach it from, and a base with no storage has none
    // reported here rather than left to gen_lvalue's contextless "Expression is not addressable",
    // which is what `$a->get()->x` used to abort with. binding the intermediate to a local is the
    // answer, and for a class it is also the answer to *who releases it* - which is why chaining
    // through a call result stays out until temporaries have owners (todo/A13)
    auto &base = node.get_base_node();
    if (base.has() && base.is_expression_node()) {
        auto *base_expr = base.unsafe_ptr<ExprNode>();
        if (!is_place_expression(*base_expr) && base_expr->result_type().has_complex_type()) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.get_member_name()),
                fmt::format(
                    "'{}' has no storage to read a member from - bind it to a variable first",
                    base_expr->result_type().get_type_desciption()));
        }
    }

    RecursiveVisitor::visitMemberAccess(node);
}

void TypeChecker::visit_instanceof_expr(InstanceOfExprNode &node)
{
    const ValueType operand_type = node.operand->result_type();

    // "structs do not have any runtime meta data ... you can also not perform any runtime reflection
    // checks on them" (CONCEPT.md). the question is not merely false for a struct, it is unanswerable:
    // there is no block and no identity word, so the value never carried an answer. a *class* operand
    // against a struct type is a different matter and folds to false, which is why only the left side
    // is checked here
    if (!operand_type.is_class()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_instanceof),
            fmt::format(
                "'instanceof' needs a class on the left - a '{}' carries no runtime type to check",
                operand_type.get_type_desciption()));
    }

    if (!node.queried_type.has_complex_type()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_instanceof),
            fmt::format(
                "'instanceof' needs a struct or a class on the right, not '{}'",
                node.queried_type.get_type_desciption()));
    }

    RecursiveVisitor::visit_instanceof_expr(node);
}

void TypeChecker::visitFunctionCallExpr(FunctionCallExprNode &node)
{
    // generic templates are resolved to concrete instances by the monomorphizer; only a
    // resolved, non-generic callee has stable parameter types to check against
    if (node.decl && !node.decl->is_generic()) {
        const auto &params = node.decl->args;

        // every argument is checked, the receiver included - `$p->m()` on a null pointer is exactly
        // the case the borrow guard below exists for - but the *number* a diagnostic reports is the
        // one the reader can count to, which is what user_arg_number answers
        if (node.arguments.size() == params.size()) {
            for (size_t i = 0; i < params.size(); i++) {
                if (!node.arguments[i] || !params[i]->has_type()) {
                    continue;
                }
                // a mismatched argument that the parser/monomorphizer could not reconcile with an
                // implicit cast is caught here directly (e.g. two distinct struct types). arguments
                // that were wrapped in an implicit cast are validated in visitTypeCast instead,
                // where the illegal conversion actually lives
                ValueType arg_type = node.arguments[i]->result_type();
                ValueType param_type = params[i]->type();

                // a borrow promises it is never null, and the declaration site already refuses
                // to seed one with null - the call site has to refuse too, or the promise only
                // holds for locals. this was a segfault the moment the callee read through it
                ExprNode *written = strip_implicit_casts(node.arguments[i]);
                if (written != nullptr && written->get_node_type() == NodeType::n_null
                    && param_type.is_pointer() && !param_type.is_nullable()) {
                    _collector.collect_issue<Issue::GenericError>(
                        code_ref_for(node.token_function_name),
                        fmt::format("argument {} of '{}' is '{}', which cannot be null",
                            node.decl->user_arg_number(i), node.decl->func_name(), param_type.get_type_desciption()));
                    continue;
                }

                if (!arg_assignable_to(arg_type, param_type)) {
                    _collector.collect_issue<Issue::ArgumentTypeMismatch>(
                        code_ref_for(node.token_function_name),
                        fmt::format(
                            "Argument {} of '{}' expects type '{}' but got '{}'",
                            node.decl->user_arg_number(i),
                            node.decl->func_name(),
                            param_type.get_type_desciption(),
                            arg_type.get_type_desciption()));
                }
            }
        }
    }

    // echo is a decl-less builtin, and its codegen has a printf conversion for every primitive and
    // nothing else. reported here so each gap is a located diagnostic instead of the uncaught codegen
    // throw it used to be. the `decl == nullptr` guard is what keeps this off a user-declared or
    // namespaced function that happens to be spelled `echo` - it has a signature, so the ordinary
    // argument checks above are the ones that apply to it
    //
    // two shapes are worth naming. an *address*, because after the adjustment pass a pointer here
    // really is an address rather than a not-yet-dereferenced read, so printing one is almost always
    // a missing read. and a *named type*, struct or class, for which there is no rendering to pick at
    // all - giving them one is todo/B6
    if (node.decl == nullptr && node.token_function_name.value() == "echo") {
        for (auto *arg : node.arguments) {
            if (arg == nullptr) {
                continue;
            }

            const ValueType type = arg->result_type();

            if (type.is_pointer()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_function_name),
                    fmt::format("cannot echo an address of type '{}' - echo prints values",
                        type.get_type_desciption()));
            }
            else if (type.has_complex_type()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_function_name),
                    fmt::format("'echo' has no way to print a '{}' - print its members instead",
                        type.get_type_desciption()));
            }
        }
    }

    // walk arguments with this call's name token as the location context, so an illegal implicit
    // cast inserted around an argument is reported at the call site
    const TokenReference *prev = _context_token;
    _context_token = &node.token_function_name;
    RecursiveVisitor::visitFunctionCallExpr(node);
    _context_token = prev;
}

void TypeChecker::visitTypeCast(TypeCastNode &node)
{
    // the parser/monomorphizer inserts implicit casts to reconcile types; if such a cast is not a
    // legal conversion (e.g. a struct where a primitive is expected) it would otherwise surface as
    // a context-free "Unsupported type cast" deep in codegen. report it here, located
    if (node.is_implcit && node.expr && _context_token) {
        ValueType from = node.expr->result_type();
        if (!arg_assignable_to(from, node.cast_to)) {
            _collector.collect_issue<Issue::InvalidTypeConversion>(
                code_ref_for(*_context_token),
                fmt::format("cannot implicitly convert '{}' to '{}'",
                    from.get_type_desciption(), node.cast_to.get_type_desciption()));
        }
    }

    RecursiveVisitor::visitTypeCast(node);
}

void TypeChecker::visitBinaryExpr(BinaryExprNode &node)
{
    // codegen (gen_binary_expr) lowers operators only over numeric and a narrow bool set; it
    // supports no operator on struct/class operands, where it would otherwise fall through to a
    // context-free codegen throw. flag exactly that unambiguous case here, located at the operator
    // undeterminable operands (unknown/void/type-param) are left to other diagnostics, and the rarer
    // per-branch primitive gaps (e.g. `%` on two bools) are left to the enriched codegen throw
    // rather than re-encoding codegen's full operator matrix and risking false positives
    if (node.lhs && node.rhs && node.op_node) {
        ValueType lhs = node.lhs->result_type();
        ValueType rhs = node.rhs->result_type();

        // comparing against null only means something on an address. `$p == null` would read
        // the int32 at address zero - exactly the crash the check is meant to prevent - so it
        // is rejected and `$p:$ == null` is the way to ask
        // (book/concept/pointers_and_refs_v2.md, "Nullability")
        const bool lhs_null = node.lhs->get_node_type() == NodeType::n_null;
        const bool rhs_null = node.rhs->get_node_type() == NodeType::n_null;

        if (lhs_null != rhs_null) {
            const ValueType &other = lhs_null ? rhs : lhs;
            // a class handle is itself the address, so it is compared directly - there is no slot to
            // peel to and `:$` on it would ask about the variable rather than the object. a struct has
            // no runtime representation that could be absent, which is why only these two answer
            if (!other.is_pointer() && !other.is_class()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.op_node->token_literal),
                    other.is_struct()
                        ? fmt::format("cannot compare '{}' against null - a struct is always there",
                            other.get_type_desciption())
                        : fmt::format("cannot compare '{}' against null - null-check the address with ':$'",
                            other.get_type_desciption()));
            }
        }

        // comparing an address against a non-address. codegen lowers a pointer comparison to an
        // icmp over two pointers, and llvm asserts outright when the operand types differ - so
        // without this the compiler aborted with "Both operands to ICmp instruction are not of
        // the same type!" and no location at all
        //
        // scoped to comparisons: `$p:$ + 1` mixes a pointer and an int legitimately, because
        // arithmetic on an address is offsetting rather than comparing
        if (!lhs_null && !rhs_null && lhs.is_pointer() != rhs.is_pointer()
            && !lhs.is_void() && !rhs.is_void()
            && node.op_node->op->is_comparison()) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.op_node->token_literal),
                fmt::format("cannot compare '{}' against '{}' - an address only compares against another address",
                    lhs.get_type_desciption(), rhs.get_type_desciption()));
        }

        // `==` and `!=` over two class handles is an address comparison, which codegen does lower -
        // it is how two references are told apart and how a null one is detected. a null operand
        // types as void here rather than as a class, so it is admitted the same way. everything
        // else on a struct or a class operand still has no lowering at all
        const bool class_identity =
            node.op_node->op->is_identity_comparison()
            && (lhs.is_class() || rhs.is_class())
            && (lhs.is_class() || lhs_null)
            && (rhs.is_class() || rhs_null);

        if (!class_identity && (lhs.has_complex_type() || rhs.has_complex_type())) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.op_node->token_literal),
                fmt::format("operator '{}' is not supported on operands of type '{}' and '{}'",
                    node.op_node->token_literal.value(),
                    lhs.get_type_desciption(),
                    rhs.get_type_desciption()));
        }
    }

    RecursiveVisitor::visitBinaryExpr(node);
}

// `const` is a promise about the storage an assignment reaches, and after the adjustment pass the
// target's shape says which level that is: a deref means the write goes *through* a pointer, so the
// pointee's const decides it, while any other place names the slot itself. the parser cannot make
// this call - writing through and re-seating are the same token sequence until the adjuster has
// inserted the deref (book/concept/pointers_and_refs_v2.md, "Const")
void TypeChecker::check_const_target(AssignNode &node)
{
    ExprNode &target = *node.target;

    if (target.get_node_type() == NodeType::n_expr_deref) {
        const ValueType pointer_type = static_cast<DerefExprNode &>(target).operand->result_type();

        if (pointer_type.is_pointer() && pointer_type.pointee().is_const()) {
            _collector.collect_issue<Issue::ConstViolation>(
                code_ref_for(node.token_assign),
                fmt::format("cannot write through '{}' - its pointee is const",
                    pointer_type.get_type_desciption()));
        }

        return;
    }

    // an initialization is the one write a const *slot* legitimately gets, and the one re-seat a const
    // *pointer* legitimately gets - both of which are decided below. what it is never entitled to is
    // the write-*through* above: a `ptr<const T>` property means the pointee is not this constructor's
    // to write, however fresh the slot holding the pointer is. so the exemption starts here rather
    // than at the call site, which used to skip this function whole
    if (node.is_initialization) {
        return;
    }

    if (!is_place_expression(target)) {
        return;
    }

    const ValueType storage = target.result_type();
    if (!storage.is_const()) {
        return;
    }

    // a const *pointer* still permits the write-through above - what it forbids is re-seating, and
    // `$p:$` is the only spelling that reaches the slot, so arriving here with a pointer means that
    if (storage.is_pointer()) {
        _collector.collect_issue<Issue::ConstViolation>(
            code_ref_for(node.token_assign),
            fmt::format("cannot re-seat '{}' - the pointer is const, only its pointee may be written",
                storage.get_type_desciption()));
        return;
    }

    const std::string name = place_description(target);
    _collector.collect_issue<Issue::ConstViolation>(
        code_ref_for(node.token_assign),
        name.empty()
            ? fmt::format("cannot assign to const storage of type '{}'", storage.get_type_desciption())
            : fmt::format("cannot assign to '{}' - it is declared const", name));
}

void TypeChecker::check_destination_fits(Destination dest, const ValueType &to, const ExprNode &value, const TokenReference &at)
{
    const ValueType from = value.result_type();

    // scoped to the destinations that have no conversion to fall back on: that is the surface where
    // a mismatch is a real error rather than a widening. `T&` widens to `ptr<T>` freely while the
    // narrowing back asserts non-nullness and needs the explicit cast
    // (book/concept/pointers_and_refs_v2.md, "Two pointer types"), and a struct slot takes nothing
    // but that struct. null answers to its own rules, and an undeterminable type to other diagnostics
    if (to.is_void() || from.is_void()
        || value.get_node_type() == NodeType::n_null
        || (!demands_exact_conversion(to) && !demands_exact_conversion(from))
        || is_implicitly_convertible(from, to)) {
        return;
    }

    // the hints are properties of the type pair rather than of the destination, so they are
    // phrased once here
    std::string hint;
    if (!to.is_pointer() && from.is_pointer() && dest == Destination::t_assignment) {
        hint = " - to change where a pointer points, assign to ':$'";
    }
    // only when the value is an address too: the cast the hint asks for narrows a nullable
    // pointer to a borrow, and there is nothing to narrow if the value is not one
    else if (from.is_pointer() && to.is_pointer() && !to.is_nullable()) {
        hint = " - write the cast explicitly to assert it is not null";
    }

    std::string message;
    switch (dest) {
        case Destination::t_declaration:
            message = fmt::format("cannot implicitly convert '{}' to '{}'{}",
                from.get_type_desciption(), to.get_type_desciption(), hint);
            break;

        case Destination::t_assignment:
            message = fmt::format("cannot assign '{}' to '{}'{}",
                from.get_type_desciption(), to.get_type_desciption(), hint);
            break;

        case Destination::t_return:
            message = fmt::format("cannot return '{}' from a function declared '{}'{}",
                from.get_type_desciption(), to.get_type_desciption(), hint);
            break;
    }

    _collector.collect_issue<Issue::InvalidTypeConversion>(code_ref_for(at), message);
}

void TypeChecker::visit_assign(AssignNode &node)
{
    if (node.target != nullptr) {
        check_const_target(node);
    }

    // the value has to fit the storage the target names, checked wherever a conversion cannot be
    // synthesized for it (demands_exact_conversion)
    //
    // this is what rejects `$p = &$b`: after the adjustment pass the target is a deref of $p,
    // so the storage is an int32 while the value is an int32& - assigning an address into the
    // pointee's slot. re-seating is spelled `$p:$ = &$b`, whose target *is* the slot
    // (book/concept/pointers_and_refs_v2.md, "Binding, writing, and re-seating")
    if (node.target && node.value_expr) {
        check_destination_fits(Destination::t_assignment, node.target->result_type(), *node.value_expr, node.token_assign);
    }

    RecursiveVisitor::visit_assign(node);
}

void TypeChecker::visitVarDecl(VarDeclNode &node)
{
    if (node.has_type() && contains_type_param(node.type())) {
        _collector.collect_issue<Issue::UnresolvedTypeParameter>(
            code_ref_for(node.token_varname),
            fmt::format(
                "The type of variable '{}' could not be resolved to a concrete type "
                "(unresolved generic type parameter)",
                node.name()));
    }

    if (node.init_expr && node.has_type()) {
        check_destination_fits(Destination::t_declaration, node.type(), *node.init_expr, node.token_varname);
    }

    // a borrow is the type that promises it is never null, so seeding one with null defeats
    // the only guarantee it carries. use ptr<T> when the absence case is real (doc L59)
    if (node.init_expr && node.init_expr->get_node_type() == NodeType::n_null
        && node.has_type() && node.type().is_pointer() && !node.type().is_nullable()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_varname),
            fmt::format("'{}' cannot be null - declare it as a nullable pointer instead",
                node.type().get_type_desciption()));
    }

    // locate any implicit cast in the initializer at the declared variable
    const TokenReference *prev = _context_token;
    _context_token = &node.token_varname;
    RecursiveVisitor::visitVarDecl(node);
    _context_token = prev;
}

};  // namespace AST
