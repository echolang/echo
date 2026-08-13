#include "AST/ASTGuardLowering.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/GuardNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

namespace AST
{

GuardLowering::GuardLowering(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef GuardLowering::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

bool GuardLowering::run_round()
{
    _changed = false;

    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            _current_file = &file;
            _hoist_count = 0;

            if (file.root != nullptr) {
                file.root->accept(*this);
            }
        }
    }

    return _changed;
}

void GuardLowering::finalize()
{
    // one more round rather than a sweep: a round inherits visitFunctionDecl's generic-body skip and
    // walks scope children, which is the tree walk this pass is required to use
    _finalizing = true;
    run_round();
    _finalizing = false;
}

void GuardLowering::visitScope(ScopeNode &node)
{
    for (size_t i = 0; i < node.children.size(); i++) {
        if (node.children[i].has_type<GuardNode>()) {
            auto *guard = node.children[i].unsafe_ptr<GuardNode>();

            if (guard != nullptr && !guard->plan_decided) {
                lower(node, i);

                // lower() inserts the hoisted subject declaration *at* `i`, so the guard is now at
                // `i + 1`. stepping over the declaration is right: it is an ordinary local and the walk
                // has nothing to do to it, and it keeps the guard from being visited twice in one round
                if (node.children[i].has_type<VarDeclNode>()) {
                    i++;
                }
            }
        }

        // re-read rather than cached: the list above may have grown
        if (i < node.children.size() && node.children[i].has()) {
            node.children[i].node()->accept(*this);
        }
    }
}

void GuardLowering::visitFunctionDecl(FunctionDeclNode &node)
{
    if (!node.is_generic()) {
        RecursiveVisitor::visitFunctionDecl(node);
    }
}

FunctionCallExprNode &GuardLowering::subject_call(
    VarDeclNode &subject,
    const std::string &name,
    const TokenReference &at
)
{
    // AST::make_unresolved_member_call owns both halves of this. the receiver rule matters here in
    // particular: the borrow arm in lower() binds `$__guardN` as a `ptr<T>`, which is what a receiver
    // wants already - addressing it again yields `ptr<ptr<T>>`, unifies against nothing, and the call is
    // then *silently never instantiated*
    return make_unresolved_member_call(*_current_module, subject, name, at);
}

bool GuardLowering::bind_payload_type(
    GuardNode &guard,
    const ValueType &payload,
    const ValueType &subject
)
{
    // an inferred binding takes the payload, keeping whatever `const` the author wrote - the parser left
    // a placeholder carrying exactly that and nothing else. an *undetermined* type is one, so the two
    // conditions are one question
    if (!guard.decl->has_type() || is_undetermined_type(guard.decl->type())) {
        const bool wants_const = guard.decl->has_type() && guard.decl->type().is_const();

        guard.decl->set_type_node(&_current_module->nodes.emplace_back<TypeNode>(
            infer_declaration_type(payload, wants_const)));

        return true;
    }

    // a *written* type is checked against what is actually inside, in AST::guard_payload_refusal's
    // wording - shared with Parser::parse_guard, which asks the same question of a `T?` as the
    // statement is read
    const std::string mismatch = guard_payload_refusal(guard.decl->type(), payload, subject);

    if (mismatch.empty()) {
        return true;
    }

    refuse(guard, guard.decl->token_varname, mismatch);

    return false;
}

void GuardLowering::refuse(GuardNode &guard, const TokenReference &at, std::string why)
{
    _collector.collect_issue<Issue::GenericError>(code_ref_for(at), std::move(why));

    // **reported and kept.** a guard's binding is read *after* the statement, so forgetting the subtree
    // would free a declaration those reads still point at - the unsound direction of
    // NodeCollection::forget. instead the binding is typed defensively so the reads check against
    // something rather than cascading, `plan_decided` is set so nothing asks again, and
    // has_critical_issues() stops the build long before codegen.
    guard.plan_decided = true;

    if (guard.decl != nullptr && !guard.decl->has_type()) {
        const ValueType subject = guard.decl->init_expr != nullptr
            ? guard.decl->init_expr->result_type()
            : ValueType::make_unknown();

        guard.decl->set_type_node(&_current_module->nodes.emplace_back<TypeNode>(subject));
    }

    if (guard.failure != nullptr && !guard.failure->has_type()) {
        guard.failure->set_type_node(
            &_current_module->nodes.emplace_back<TypeNode>(ValueType::make_unknown()));
    }

    _changed = true;
}

void GuardLowering::lower(ScopeNode &scope, size_t index)
{
    auto *guard = scope.children[index].unsafe_ptr<GuardNode>();

    if (guard == nullptr || guard->decl == nullptr || guard->decl->init_expr == nullptr) {
        return;
    }

    ExprNode *subject = guard->decl->init_expr;
    const ValueType subject_type = subject->result_type();

    const UnwrapLookup look =
        unwrap_plan_for(subject_type, _collector.core_types, _collector.type_registry);

    if (look.result == UnwrapLookup::Result::t_pending) {
        // **out of rounds is out of answers** - see finalize(). the message is only for the case where
        // nothing else explained it, which has_critical_issues() is already the compiler's answer to
        if (_finalizing) {
            if (_collector.has_critical_issues()) {
                guard->plan_decided = true;
            }
            else {
                refuse(*guard, guard->token, fmt::format(
                    "'{}' never got a type, so there is nothing to unwrap.",
                    subject_type.get_type_desciption()));
            }
        }

        return;  // ask again next round
    }

    if (look.result == UnwrapLookup::Result::t_refused) {
        refuse(*guard, guard->token, look.refusal);
        return;
    }

    const UnwrapPlan &plan = look.plan;

    // **the failure binding needs a contract, and the `$e` is where that belongs** - it is the token that
    // asked for one. the two refusals are separate because the mistakes are not the same mistake: a
    // nullable has no reason to give, while an unwrappable that declares no `failable` might have been
    // meant to
    if (guard->failure != nullptr && !plan.failure_type.has_value()) {
        const std::string failable_name = _collector.core_types.spelling(CoreTypeKind::t_failable);

        // the token value already carries its `$`, so the name is interpolated bare
        const std::string name = guard->failure->token_varname.value();

        if (plan.kind == UnwrapSource::t_builtin_nullable) {
            refuse(*guard, guard->failure->token_varname, fmt::format(
                "a '{}' records only that the value is absent, so there is no reason to bind to '{}'. "
                "'else ({})' needs a subject declaring '{}<E>' - drop the '({})' to guard a 'T?'.",
                subject_type.get_type_desciption(), name, name, failable_name, name));
        }
        else {
            refuse(*guard, guard->failure->token_varname, fmt::format(
                "'{}' unwraps, but it declares no '{}' - so there is no '{}' to bind. declare one on "
                "it, or drop the '({})'.",
                subject_type.get_type_desciption(), failable_name, name, name));
        }

        return;
    }

    // **the deferred nullable case mints nothing at all.** a `T?` that only became one after
    // substitution runs the same four lines the parser's immediate path runs, which is also what fixes a
    // latent bug: `$v = guard $w` in a template over `T = int32?` used to type the binding
    // `unwrapped_type_of(T)` = `T`, substituting back to `int32?` - one level too nullable
    if (plan.kind == UnwrapSource::t_builtin_nullable) {
        if (!bind_payload_type(*guard, plan.payload_type, subject_type)) {
            return;
        }

        guard->plan_decided = true;
        _changed = true;
        return;
    }

    // ---- the protocol case ----

    // **the subject is hoisted into a sibling declaration, spliced ahead of the guard.** a sibling and
    // deliberately not a wrapper scope: the *binding* has to live to the end of the enclosing scope, and
    // a wrapper would have AST::OwnershipPass drop it at the guard's own closing brace. as an ordinary
    // local of this scope it needs no ownership rule, no codegen and no drop rule - gen_var_decl seats it
    // and the frame ends it
    auto &subject_decl = _current_module->nodes.emplace_back<VarDeclNode>(
        _current_module->make_virtual_token(
            fmt::format("$__guard{}", _hoist_count++), Token::Type::t_varname, guard->token),
        nullptr);

    if (is_place_expression(*subject)) {
        // **a borrow, not a copy**, and the difference is observable: `unwrap()` is not declared const,
        // so it may write, and it has to write through to the value the author named
        subject_decl.init_expr = &_current_module->nodes.emplace_back<AddrOfExprNode>(subject);
        subject_decl.set_type_node(&_current_module->nodes.emplace_back<TypeNode>(
            ValueType::make_pointer(subject_type, false)));
    }
    else {
        // a subject produced by a call. `$__guardN` owns it, and the ordinary frame machinery drops it
        subject_decl.init_expr = subject;
        subject_decl.set_type_node(&_current_module->nodes.emplace_back<TypeNode>(subject_type));
    }

    // **the subject moved into the hoisted declaration and is used exactly once**, so "one subtree per
    // use" holds by construction - AST::ForeachLowering's `loop->source = nullptr` is the same line for
    // the same reason. the guard now branches on `presence_test` instead
    guard->decl->init_expr = nullptr;

    scope.children.insert(
        scope.children.begin() + static_cast<long>(index), make_ref(subject_decl));

    // the name has to resolve for the calls minted below, and this is the scope they live in
    scope.declare_variable(subject_decl);

    guard->presence_test = &subject_call(subject_decl, "has_value", guard->token);

    // **the deref is written here rather than hoped for.** `unwrap()` hands back `V&`, and this is the
    // round in which AST::OwnershipPass decides the binding's copy - so the edge it reads has to be the
    // one that will actually be read. AST::ForeachLowering writes the same deref over `current()` for
    // the same reason
    guard->bound_value = &_current_module->nodes.emplace_back<DerefExprNode>(
        &subject_call(subject_decl, "unwrap", guard->decl->token_varname));

    if (!bind_payload_type(*guard, plan.payload_type, subject_type)) {
        return;
    }

    if (guard->failure != nullptr) {
        guard->failure->init_expr = &_current_module->nodes.emplace_back<DerefExprNode>(
            &subject_call(subject_decl, "failure", guard->failure->token_varname));

        guard->failure->set_type_node(
            &_current_module->nodes.emplace_back<TypeNode>(plan.failure_type.value()));
    }

    guard->plan_decided = true;
    _changed = true;
}

};
