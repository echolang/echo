#include "AST/ASTPointerAdjuster.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NullNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "AST/WhileStatementNode.h"

namespace AST
{

PointerAdjuster::PointerAdjuster(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef PointerAdjuster::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

void PointerAdjuster::run()
{
    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();
        for (auto &file : module_ptr->files()) {
            _current_file = &file;
            if (file.root) {
                adjust(file.root);
            }
        }
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

ExprNode *PointerAdjuster::as_value(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    // a peeled expression is already the value it means - the pointer itself - so it keeps its
    // place and gets no deref. this is the entire implementation of `:$`
    if (expr->get_node_type() == NodeType::n_expr_peel) {
        adjust(expr);
        return strip_peel(expr);
    }

    adjust(expr);

    // only a place holding a pointer needs the deref. an AddrOfExprNode is a pointer too, but
    // it is already the value it means - `&$x` yields an address, it does not read through one
    if (!is_place_expression(*expr) || !expr->result_type().is_pointer()) {
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

    if (wanted.is_pointer() && expr == nullptr) {
        return nullptr;
    }

    return as_value(expr);
}

ExprNode *PointerAdjuster::adjust_place(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    adjust(expr);
    return strip_peel(expr);
}

void PointerAdjuster::bind_null_operand(ExprNode *maybe_null, ExprNode *other)
{
    if (maybe_null == nullptr || other == nullptr) {
        return;
    }

    if (maybe_null->get_node_type() != NodeType::n_null) {
        return;
    }

    auto *null_node = static_cast<NullNode *>(maybe_null);
    if (null_node->is_bound()) {
        return;
    }

    ValueType other_type = other->result_type();
    if (other_type.is_pointer()) {
        null_node->bound_type = other_type;
    }
}

void PointerAdjuster::adjust(Node *node)
{
    if (node == nullptr) {
        return;
    }

    switch (node->get_node_type())
    {
        case NodeType::n_scope:
        {
            auto *scope = static_cast<ScopeNode *>(node);
            for (auto &child : scope->children) {
                adjust(child.node());
            }
            break;
        }

        case NodeType::n_func_decl:
        {
            auto *fn = static_cast<FunctionDeclNode *>(node);
            // a template's body is only meaningful once cloned into a concrete instance
            if (!fn->is_generic()) {
                FunctionDeclNode *prev = _current_function;
                _current_function = fn;
                adjust(fn->body);
                _current_function = prev;
            }
            break;
        }

        case NodeType::n_vardecl:
        {
            auto *decl = static_cast<VarDeclNode *>(node);
            // a declaration *binds*: `ptr<int32> $p = &$a;` stores the address, so the
            // initializer keeps its pointer when the declared type is a pointer too
            decl->init_expr = as_value_for(
                decl->init_expr, decl->has_type() ? decl->type() : ValueType::make_unknown());
            break;
        }

        case NodeType::n_assign:
        {
            auto *assign = static_cast<AssignNode *>(node);
            // the target goes through as_value like any other read: a plain pointer target
            // gains the deref that write-through means, while `$p:$` keeps the slot it names.
            // that is the whole difference between writing through a pointer and re-seating it
            assign->target = as_value(assign->target);

            // the value is then read to fit whatever storage the target turned out to name.
            // for `ptr<ptr<int>> $out`, `$out = $target` writes the caller's *pointer*, so the
            // value keeps its address instead of being read through
            assign->value_expr = as_value_for(
                assign->value_expr,
                assign->target != nullptr ? assign->target->result_type() : ValueType::make_unknown());
            break;
        }

        case NodeType::n_expr_binary:
        {
            auto *bin = static_cast<BinaryExprNode *>(node);
            bin->lhs = as_value(bin->lhs);
            bin->rhs = as_value(bin->rhs);

            // null has no type of its own, so in a comparison it takes the other operand's.
            // done here rather than in the parser because the other side's pointer-ness is
            // only settled once the derefs above are in place
            bind_null_operand(bin->lhs, bin->rhs);
            bind_null_operand(bin->rhs, bin->lhs);
            break;
        }

        case NodeType::n_expr_unary:
        {
            auto *un = static_cast<UnaryExprNode *>(node);
            un->expr = as_value(un->expr);
            break;
        }

        case NodeType::n_expr_call:
        {
            auto *call = static_cast<FunctionCallExprNode *>(node);
            for (size_t i = 0; i < call->arguments.size(); i++) {
                auto *&arg = call->arguments[i];

                // an argument already wrapped in an address-of was borrowed deliberately,
                // by the coercion pass or by the user writing `&$x`; leave it as the address
                if (arg != nullptr && arg->get_node_type() == NodeType::n_expr_addrof) {
                    auto *addr = static_cast<AddrOfExprNode *>(arg);
                    addr->operand = adjust_place(addr->operand);
                    continue;
                }

                // otherwise the parameter decides how far to read: a pointer parameter takes
                // the address as it is, anything else reads through. that is also where the
                // generic decay lands - an inferred `T` is not a pointer, so a pointer
                // argument is read to its pointee
                ValueType wanted = ValueType::make_unknown();
                if (call->decl != nullptr && i < call->decl->args.size() && call->decl->args[i]->has_type()) {
                    wanted = call->decl->args[i]->type();
                }

                arg = as_value_for(arg, wanted);
            }
            break;
        }

        case NodeType::n_expr_addrof:
        {
            // "no transparency peeling": the operand of `&` is wanted as a place
            auto *addr = static_cast<AddrOfExprNode *>(node);
            addr->operand = adjust_place(addr->operand);
            break;
        }

        case NodeType::n_expr_deref:
        {
            auto *deref = static_cast<DerefExprNode *>(node);
            deref->operand = adjust_place(deref->operand);
            break;
        }

        case NodeType::n_expr_index:
        {
            // the base is wanted as the address it holds, so it keeps its pointer and gets no
            // deref - `$p:$[1]` offsets from $p's address, it does not read through it first
            auto *index_expr = static_cast<IndexExprNode *>(node);
            index_expr->base = adjust_place(index_expr->base);
            index_expr->index = as_value(index_expr->index);
            break;
        }

        case NodeType::n_expr_peel:
        {
            auto *peel = static_cast<PointerValueNode *>(node);
            peel->operand = adjust_place(peel->operand);
            break;
        }

        case NodeType::n_member_access:
        {
            // the base is wanted as a place; `->` reaching through a pointer is gen_place's job
            auto *access = static_cast<MemberAccessNode *>(node);
            adjust(access->get_base_node().node());
            break;
        }

        case NodeType::n_type_cast:
        {
            auto *cast = static_cast<TypeCastNode *>(node);
            cast->expr = as_value(cast->expr);
            break;
        }

        case NodeType::n_func_return:
        {
            // a return fits its value to the declared return type, exactly as a declaration
            // and an assignment do. a `T&` return hands back the address, so reading through
            // it here would return the pointee where the signature promised the pointer -
            // which llvm's verifier rejects outright ("return type does not match operand")
            auto *ret = static_cast<ReturnNode *>(node);
            ret->expr = as_value_for(
                ret->expr,
                _current_function != nullptr ? _current_function->get_return_type() : ValueType::make_unknown());
            break;
        }

        case NodeType::n_if_statement:
        {
            auto *stmt = static_cast<IfStatementNode *>(node);
            stmt->condition = as_value(stmt->condition);
            adjust(stmt->if_scope);
            adjust(stmt->else_scope);
            break;
        }

        case NodeType::n_while_statement:
        {
            auto *stmt = static_cast<WhileStatementNode *>(node);
            stmt->condition = as_value(stmt->condition);
            adjust(stmt->loop_scope);
            break;
        }

        default:
            // leaves: literals, operators, var references, types. nothing to rewrite
            break;
    }
}

}
