#include "AST/ASTTypeChecker.h"

#include "AST/ASTModule.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/StructNode.h"
#include "AST/MemberAccessNode.h"

#include <fmt/core.h>

namespace AST
{

// resolves the value type of a member-access base (either a variable reference or a nested member
// access). returns an unknown type for any other base kind, which callers treat as "don't check".
static ValueType base_type_of(MemberAccessNode &node)
{
    auto &base = node.get_base_node();
    if (base.has_type<VarRefNode>()) {
        return base.get<VarRefNode>().result_type();
    }
    if (base.has_type<MemberAccessNode>()) {
        return base.get<MemberAccessNode>().result_type();
    }
    return ValueType::make_unknown();
}

// numeric/primitive conversions are inserted by the parser/monomorphizer as casts, so only a
// fundamental kind mismatch (struct vs primitive, or two distinct struct identities) is a real
// argument error here. undeterminable types (void/unknown) are left to other diagnostics.
static bool arg_assignable_to(const ValueType &arg, const ValueType &param)
{
    if (arg.get_kind() == ValueTypeKind::t_unknown || param.get_kind() == ValueTypeKind::t_unknown) {
        return true;
    }
    if (arg.is_void()) {
        return true;
    }
    if (arg.is_primitive() && param.is_primitive()) {
        return true;
    }
    if ((arg.is_struct() || arg.is_class()) && (param.is_struct() || param.is_class())) {
        return arg.get_complex_type() == param.get_complex_type();
    }
    return false;
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
    // meaningful once cloned into a concrete instance, which is checked separately.
    if (node.is_generic()) {
        return;
    }
    RecursiveVisitor::visitFunctionDecl(node);
}

void TypeChecker::visitStructDecl(StructDeclNode &node)
{
    // a generic struct template's property types legitimately mention its type parameters (the T
    // in `struct Box<T> { T $value; }`); it is only meaningful once instantiated with concrete
    // types. concrete/non-generic struct declarations are still checked.
    if (node.is_generic()) {
        return;
    }
    RecursiveVisitor::visitStructDecl(node);
}

void TypeChecker::visitMemberAccess(MemberAccessNode &node)
{
    ValueType base_type = base_type_of(node);
    if (base_type.is_struct() || base_type.is_class()) {
        ComplexType *complex = base_type.get_complex_type();
        const std::string member = node.get_member_name().value();
        if (complex && !complex->has_property(member)) {
            _collector.collect_issue<Issue::UnknownMember>(
                code_ref_for(node.get_member_name()),
                member,
                complex->name.value_or("<anonymous>"));
        }
    }

    RecursiveVisitor::visitMemberAccess(node);
}

void TypeChecker::visitFunctionCallExpr(FunctionCallExprNode &node)
{
    // generic templates are resolved to concrete instances by the monomorphizer; only a
    // resolved, non-generic callee has stable parameter types to check against.
    if (node.decl && !node.decl->is_generic()) {
        const auto &params = node.decl->args;
        if (node.arguments.size() == params.size()) {
            for (size_t i = 0; i < params.size(); i++) {
                if (!node.arguments[i] || !params[i]->has_type()) {
                    continue;
                }
                // a mismatched argument that the parser/monomorphizer could not reconcile with an
                // implicit cast is caught here directly (e.g. two distinct struct types). arguments
                // that were wrapped in an implicit cast are validated in visitTypeCast instead,
                // where the illegal conversion actually lives.
                ValueType arg_type = node.arguments[i]->result_type();
                ValueType param_type = params[i]->type();
                if (!arg_assignable_to(arg_type, param_type)) {
                    _collector.collect_issue<Issue::ArgumentTypeMismatch>(
                        code_ref_for(node.token_function_name),
                        fmt::format(
                            "Argument {} of '{}' expects type '{}' but got '{}'",
                            i + 1,
                            node.decl->func_name(),
                            param_type.get_type_desciption(),
                            arg_type.get_type_desciption()));
                }
            }
        }
    }

    // walk arguments with this call's name token as the location context, so an illegal implicit
    // cast inserted around an argument is reported at the call site.
    const TokenReference *prev = _context_token;
    _context_token = &node.token_function_name;
    RecursiveVisitor::visitFunctionCallExpr(node);
    _context_token = prev;
}

void TypeChecker::visitTypeCast(TypeCastNode &node)
{
    // the parser/monomorphizer inserts implicit casts to reconcile types; if such a cast is not a
    // legal conversion (e.g. a struct where a primitive is expected) it would otherwise surface as
    // a context-free "Unsupported type cast" deep in codegen. report it here, located.
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
    // context-free codegen throw. flag exactly that unambiguous case here, located at the operator.
    // undeterminable operands (unknown/void/type-param) are left to other diagnostics, and the rarer
    // per-branch primitive gaps (e.g. `%` on two bools) are left to the enriched codegen throw
    // rather than re-encoding codegen's full operator matrix and risking false positives.
    if (node.lhs && node.rhs && node.op_node) {
        ValueType lhs = node.lhs->result_type();
        ValueType rhs = node.rhs->result_type();
        if (lhs.is_struct() || lhs.is_class() || rhs.is_struct() || rhs.is_class()) {
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

    // locate any implicit cast in the initializer at the declared variable.
    const TokenReference *prev = _context_token;
    _context_token = &node.token_varname;
    RecursiveVisitor::visitVarDecl(node);
    _context_token = prev;
}

}  // namespace AST
