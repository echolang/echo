#include "AST/ASTTypeChecker.h"

#include "AST/ASTOperatorSemantics.h"

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
#include "AST/ASTArgumentFit.h"
#include "AST/ASTBuiltin.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/LiteralValueNode.h"

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
// **does this destination admit a `null`?** null, and the answer, in one place.
//
// `check_destination_fits` exempts a null value on purpose - "null answers to its own rules" - and those
// rules then got spelled at every arrival site that cared. three of the five were missing the callable
// case, which reached codegen as a null aggregate and either crashed the compiler or produced a value
// that faults when called
//
// answers with the reason rather than a bool, so each site can frame it for the destination it is
static const char *null_rejection_reason(const ValueType &to)
{
    // a borrow is the type that promises it is never null, so seeding one with null defeats the only
    // guarantee it carries (book/concept/pointers_and_refs_v2.md, "Two pointer types")
    if (to.is_pointer() && !to.is_nullable()) {
        return "declare it as a nullable pointer instead";
    }

    // a callable's *environment* slot is nullable - that is how a non-capturing closure is represented -
    // but its function slot is not, so there is no null callable that could be tested before calling
    if (to.is_callable()) {
        return "a callable has no empty value, so give it a function";
    }

    return nullptr;
}

static bool demands_exact_conversion(const ValueType &type)
{
    // a callable joins the list for the same reason a pointer is on it: there is no conversion between
    // two signatures. leaving it off would let a cast be silently accepted between two callables that
    // agree on nothing, and the only thing that catches a wrong `fn` slot afterwards is a crash
    return type.is_pointer() || type.has_complex_type() || type.is_callable();
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

// can this argument reach this parameter? one rule, AST::argument_fit, which is also what the
// overload matcher ranks with and what the implicit borrow is decided by - so a call this pass
// accepts is a call resolution could have chosen, and vice versa. this used to be a fourth
// hand-written copy of the same case analysis, and it disagreed with argument_fit about the borrow
// arm (which additionally requires the argument to be a place)
//
// numeric conversions are inserted as casts by AST::CallResolver, so a t_conversion answer is a
// legal argument here; only t_none is a real error. an undeterminable type answers t_undetermined,
// which is how "no information yet" stays out of this pass's diagnostics
//
// `expr` is the argument as written, or null when only its type is available. passing it admits
// t_borrow - the parameter is a borrow and this is a place, so an address would be taken - which is
// right at a call site and wrong for a cast, because a cast is not an address-of. the two callers
// differ on exactly that
static bool arg_assignable_to(const ValueType &arg, const ExprNode *expr, const ValueType &param)
{
    return argument_fit(arg, expr, param) != ArgumentFit::t_none;
}

// the same rule asked about an implicit cast rather than an argument, named so the intent is not a
// null pointer the reader has to interpret: no expression is offered, so the borrow arm is declined,
// because a cast is not an address-of
static bool implicit_conversion_is_legal(const ValueType &from, const ValueType &to)
{
    return arg_assignable_to(from, nullptr, to);
}

// looks through the implicit casts the parser and monomorphizer wrap around an argument, to the
// expression the user actually wrote. `null` is the case that needs it: the null-specific rules
// all test for the raw n_null tag, and a cast inserted to reconcile the argument with its
// parameter hides that tag behind an n_type_cast
static const ExprNode *strip_implicit_casts(const ExprNode *expr)
{
    while (expr != nullptr && expr->get_node_type() == NodeType::n_type_cast) {
        const auto *cast = static_cast<const TypeCastNode *>(expr);
        if (!cast->is_implcit) {
            break;
        }
        expr = cast->expr;
    }
    return expr;
}

// "did the user write null here" - the entry condition every null rule shares, paired one to one
// with null_rejection_reason. spelled once so an arrival site cannot get the rule right and the
// question wrong: the sites that skipped the strip judged a cast-wrapped null differently
static bool is_written_null(const ExprNode *expr)
{
    const ExprNode *written = strip_implicit_casts(expr);
    return written != nullptr && written->get_node_type() == NodeType::n_null;
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

        // and the same for a returned null, which check_destination_fits also waves through
        if (is_written_null(node.expr)) {
            if (const char *reason = null_rejection_reason(declared)) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_return.value()),
                    fmt::format("cannot return null as '{}' - {}", declared.get_type_desciption(), reason));
            }
        }

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

// `ref_count<T>(T& $handle)` infers T from anything, so overload resolution admits `ref_count($aStruct)`
// and nothing below it says no. reported here for the reason check_abort_message's rule is: the only
// other reader is ExprCodegen, which throws an *internal compiler error* with no source location, and
// that is not the user's mistake to read
void TypeChecker::check_ref_count_argument(FunctionCallExprNode &node)
{
    if (!node.decl->is_builtin()
        || builtin_kind_for(node.decl->builtin.value()) != BuiltinKind::t_ref_count) {
        return;
    }

    if (node.arguments.empty() || node.arguments[0] == nullptr) {
        return;
    }

    // the parameter is `T&`, so the argument arrives as the address of the slot holding the handle -
    // one load short of the handle itself, which is what codegen reads through. that indirection is the
    // point: taking the handle by value would retain it and answer one too high every time
    const ValueType argument_type = node.arguments[0]->result_type();
    const ValueType handle_type = value_type_of(argument_type);

    // an argument still generic is not this pass's to judge - the monomorphizer reports an
    // uninstantiated call itself, and a bare type parameter here means nothing was decided yet
    if (is_undetermined_type(handle_type) || handle_type.is_type_param()) {
        return;
    }

    if (!handle_type.is_class()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_function_name),
            fmt::format("'ref_count' needs a class handle, not '{}' - only a class carries a "
                "reference count", handle_type.get_type_desciption()));
    }
}

void TypeChecker::check_abort_message(FunctionCallExprNode &node)
{
    if (!node.decl->is_builtin()) {
        return;
    }

    // which argument the message is comes from AST::builtin_message_index, shared with the
    // ExprCodegen site that folds it - spelled here as well, the two could check one argument and
    // fold another, and a message that is not a literal folds to *nothing* rather than to an error
    const auto index = builtin_message_index(builtin_kind_for(node.decl->builtin.value()));

    if (!index.has_value() || node.arguments.size() <= *index) {
        return;
    }

    ExprNode *message = node.arguments[*index];

    // an argument whose *type* is already wrong has been reported - directly by the per-argument
    // walk above, or by visitTypeCast when the resolver wrapped it to make it fit. saying it is
    // also not a literal is two diagnostics for one mistake, and the shape is the less useful one.
    //
    // asked of the expression the user *wrote*, through the same strip is_written_null uses: a
    // legal cast around a perfectly good literal must not read as "not a literal"
    const ExprNode *written = strip_implicit_casts(message);
    if (written == nullptr) {
        return;
    }

    if (*index < node.decl->args.size() && node.decl->args[*index]->has_type()
        && !arg_assignable_to(written->result_type(), message, node.decl->args[*index]->type())) {
        return;
    }

    // the message is folded into a constant at the call site, together with the source location -
    // that is what makes these builtins rather than library functions, and it is why the text has
    // to be readable at compile time. lifts when there is a `string` type to hand one at runtime
    if (!literal_string_value(message).has_value()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(node.token_function_name),
            fmt::format("the message of '{}' must be a string literal - it is folded into the "
                "binary along with the source location, so it has to be known at compile time",
                node.decl->func_name()));
    }
}

void TypeChecker::check_call_argument(
    ExprNode *argument,
    const ValueType &param_type,
    size_t arg_number,
    const std::string &callee_name,
    const TokenReference &at)
{
    // the declaration site already refuses to seed a non-nullable parameter with null - the call site
    // has to refuse too, or the promise only holds for locals. this was a segfault the moment the
    // callee read through it
    if (is_written_null(argument)) {
        if (const char *reason = null_rejection_reason(param_type)) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(at),
                fmt::format("argument {} of '{}' is '{}', which cannot be null - {}",
                    arg_number, callee_name, param_type.get_type_desciption(), reason));
        }
        return;
    }

    // a mismatched argument that the parser/monomorphizer could not reconcile with an implicit cast is
    // caught here directly (e.g. two distinct struct types). one that *was* wrapped in an implicit cast
    // is validated in visitTypeCast instead, where the illegal conversion actually lives
    //
    // the argument as written is passed, so this scores it exactly as the matcher did
    const ValueType arg_type = argument->result_type();

    if (!arg_assignable_to(arg_type, argument, param_type)) {
        _collector.collect_issue<Issue::ArgumentTypeMismatch>(
            code_ref_for(at),
            fmt::format(
                "Argument {} of '{}' expects type '{}' but got '{}'",
                arg_number,
                callee_name,
                param_type.get_type_desciption(),
                arg_type.get_type_desciption()));
    }
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

                check_call_argument(
                    node.arguments[i],
                    params[i]->type(),
                    node.decl->user_arg_number(i),
                    node.decl->func_name(),
                    node.token_function_name);
            }
        }

        check_abort_message(node);
        check_ref_count_argument(node);
    }

    // echo is a decl-less builtin, and its codegen has a printf conversion for every primitive and
    // nothing else. reported here so each gap is a located diagnostic instead of the uncaught codegen
    // throw it used to be. AST::is_print_call owns the recognition, including the "has a declaration,
    // so the ordinary argument checks above apply instead" half of it
    //
    // two shapes are worth naming. an *address*, because after the adjustment pass a pointer here
    // really is an address rather than a not-yet-dereferenced read, so printing one is almost always
    // a missing read. and a *named type*, struct or class, for which there is no rendering to pick at
    // all - giving them one is todo/B6
    if (is_print_call(node)) {
        for (auto *arg : node.arguments) {
            if (arg == nullptr) {
                continue;
            }

            const ValueType type = arg->result_type();

            // the one complex type `echo` prints, so it is admitted ahead of the blanket refusal below.
            // ExprCodegen::gen_echo_string is the other half of this rule and the two have to agree, or
            // a program is either rejected for something that works or lowered by a path that throws
            if (_collector.core_types.is_string_like(type)) {
                continue;
            }

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
            else if (type.is_callable()) {
                // reported here for the reason the two above are: ExprCodegen has no printf conversion
                // for it and throws an *internal compiler error*, which is not the user's mistake to read
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_function_name),
                    fmt::format("'echo' has no way to print a '{}' - call it and print the result",
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

void TypeChecker::visit_indirect_call_expr(IndirectCallExprNode &node)
{
    const ValueType callee_type = node.callee_type();

    // the callee's *signature* is the parameter list here - there is no declaration to walk. the shape
    // ("this is not callable") and the arity are the parser's, reported where the call was written; what
    // is left is whether each argument reaches its parameter, which is the same question a direct call
    // asks and the same one answer
    if (callee_type.is_callable()) {
        const auto &signature = callee_type.signature();

        if (node.arguments.size() == signature.parameter_types.size()) {
            // the callee is named by its *type* - there is no declaration to take a name from. built
            // once: a callable's description recurses through its return and every parameter
            const std::string callee_name = callee_type.get_type_desciption();

            for (size_t i = 0; i < node.arguments.size(); i++) {
                if (node.arguments[i] == nullptr) {
                    continue;
                }

                // an indirect call has no implicit parameter, so the position a reader counts to is the
                // index
                check_call_argument(
                    node.arguments[i],
                    signature.parameter_types[i],
                    i + 1,
                    callee_name,
                    node.token);
            }
        }
    }

    RecursiveVisitor::visit_indirect_call_expr(node);
}

void TypeChecker::visit_closure_expr(ClosureExprNode &node)
{
    // capture is by value, and a copy of an owning value is a whole taxonomy - a retain, a copy
    // constructor, or nothing that exists at all. the environment's teardown is uniform precisely
    // because it holds no owner: one `__eco_release_env` thunk and no deinit, so an owner admitted here
    // is a leak rather than a wrong destructor. see todo/A27
    //
    // here rather than at the capture site in the parser, where the read is written: the captured
    // variable's type is not final until the monomorphizer has settled the call it was inferred from,
    // so `$b = Box<int32>(5)` was still a `Box<T>` when the parser saw it - and a bare type parameter
    // owns nothing, which is how an owning capture used to pass unnoticed
    if (node.environment_type != nullptr) {
        for (size_t i = 0; i < node.environment_type->property_count(); i++) {
            const ComplexType::Property &property = node.environment_type->get_property(i);

            if (!needs_destruction(property.type)) {
                continue;
            }

            // the property name *is* the variable's name - it is what the body's `$__env->name` read
            // resolves through - so the diagnostic can name the capture without a second list to keep
            // in step. located at the literal, which is where the copy would be made
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.token),
                fmt::format(
                    "'{}' is a '{}', which owns a resource. Capturing an owning value is not supported "
                    "yet - pass it as a parameter instead.",
                    property.name, property.type.get_type_desciption()));
        }
    }

    RecursiveVisitor::visit_closure_expr(node);
}

void TypeChecker::visitTypeCast(TypeCastNode &node)
{
    // the parser/monomorphizer inserts implicit casts to reconcile types; if such a cast is not a
    // legal conversion (e.g. a struct where a primitive is expected) it would otherwise surface as
    // a context-free "Unsupported type cast" deep in codegen. report it here, located
    if (node.is_implcit && node.expr && _context_token) {
        ValueType from = node.expr->result_type();
        if (!implicit_conversion_is_legal(from, node.cast_to)) {
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

        // **the one predicate**, AST::binary_has_builtin_meaning, which the parser reads to decide
        // whether to look for a declared `operator` and this reads to report that none was found. it
        // used to be spelled out here, and the parser asking the same question its own way is exactly
        // how the two would come to different answers - one of them silently
        //
        // the operands are handed over as **adjusted** facts: this pass runs after PointerAdjuster, so
        // every deref is already a node and result_type() is the truth. the parser normalizes
        // differently, and that asymmetry is the reason those are two named constructors
        // an undeterminable operand needs no guard here: has_complex_type() is false for unknown, void
        // and a bare type parameter, so the predicate already answers "there is a meaning" for them and
        // leaves the diagnostic to whichever pass actually knows what went wrong
        if (!binary_has_builtin_meaning(
                node.op_node->op, adjusted_operand(node.lhs), adjusted_operand(node.rhs))) {

            // **a declared operator that did not fire** is a different thing to say, and the only
            // place it can be said. the parser decides from the operand types it can see, so inside a
            // generic body it saw `T`, took the built-in path, and this node is the substituted clone -
            // a use site that looks like it should have worked. see todo/A32
            const bool declared_but_unreached = node.op_node->op->is_declared()
                && node.op_node->op->has_fixity(OpFixity::t_infix);

            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.op_node->token_literal),
                declared_but_unreached
                    ? fmt::format(
                        "operator '{}' is declared for '{}' and '{}', but an operator applied to a "
                        "type parameter is not resolved yet - the operand types are only known after "
                        "substitution. Call the operator's function form instead. See todo/A32.",
                        node.op_node->op->spelling,
                        lhs.get_type_desciption(),
                        rhs.get_type_desciption())
                    : fmt::format("operator '{}' is not supported on operands of type '{}' and '{}'",
                        node.op_node->op->spelling,
                        lhs.get_type_desciption(),
                        rhs.get_type_desciption()));
        }
    }

    RecursiveVisitor::visitBinaryExpr(node);
}

// there was no unary arm here at all, so `-$point` fell through to a context-free codegen throw with
// no location and nothing for the user to act on. the same predicate answers it, and the same
// "an operand that says nothing is somebody else's diagnostic" rule applies
void TypeChecker::visitUnaryExpr(UnaryExprNode &node)
{
    if (node.expr != nullptr) {
        const Operator *op = _collector.operators.get_operator(node.token_operator);
        const OperandFacts operand = adjusted_operand(node.expr);

        if (!unary_has_builtin_meaning(op, operand)) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.token_operator),
                fmt::format("operator '{}' is not supported on an operand of type '{}'",
                    node.token_operator.value(),
                    operand.type.get_type_desciption()));
        }
    }

    RecursiveVisitor::visitUnaryExpr(node);
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
        || is_written_null(&value)
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
        const ValueType target_type = node.target->result_type();

        // check_destination_fits waves a null value through, so the rule for one is asked here
        if (is_written_null(node.value_expr)) {
            if (const char *reason = null_rejection_reason(target_type)) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(node.token_assign),
                    fmt::format("cannot assign null to '{}' - {}",
                        target_type.get_type_desciption(), reason));
            }
        }

        check_destination_fits(Destination::t_assignment, target_type, *node.value_expr, node.token_assign);
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

    if (node.has_type() && is_written_null(node.init_expr)) {
        if (const char *reason = null_rejection_reason(node.type())) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(node.token_varname),
                fmt::format("'{}' cannot be null - {}", node.type().get_type_desciption(), reason));
        }
    }

    // locate any implicit cast in the initializer at the declared variable
    const TokenReference *prev = _context_token;
    _context_token = &node.token_varname;
    RecursiveVisitor::visitVarDecl(node);
    _context_token = prev;
}

};  // namespace AST
