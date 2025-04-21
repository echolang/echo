#include "AST/ASTForeachLowering.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTMutation.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"
#include "AST/ForeachNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/WhileStatementNode.h"

#include <fmt/core.h>

namespace AST
{

ForeachLowering::ForeachLowering(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef ForeachLowering::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

bool ForeachLowering::run_round()
{
    _changed = false;

    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            _current_file = &file;

            if (file.root != nullptr) {
                file.root->accept(*this);
            }
        }
    }

    return _changed;
}

void ForeachLowering::finalize()
{
    // one more round, rather than a sweep: a round inherits visitFunctionDecl's generic-body skip and
    // walks scope children, which is the tree walk this pass is required to use - NodeCollection owns
    // a detached node forever, so an of_type sweep would blame loops that were lowered away
    _finalizing = true;
    run_round();
    _finalizing = false;
}

void ForeachLowering::visitScope(ScopeNode &node)
{
    for (size_t i = 0; i < node.children.size(); i++) {
        if (node.children[i].has_type<ForeachNode>()) {
            lower(node, i);
        }

        // re-read rather than cached: lower() reseats the edge, and the wrapper it left behind is what
        // has to be descended into so a nested loop lowers in this same round
        if (node.children[i].has()) {
            node.children[i].node()->accept(*this);
        }
    }
}

void ForeachLowering::visitFunctionDecl(FunctionDeclNode &node)
{
    if (!node.is_generic()) {
        RecursiveVisitor::visitFunctionDecl(node);
    }
}

FunctionCallExprNode &ForeachLowering::iterator_call(
    VarDeclNode &iterator, const std::string &name, const TokenReference &at)
{
    auto &var = _current_module->nodes.emplace_back<VarNode>(&iterator, iterator.token_varname);
    auto &var_ref = _current_module->nodes.emplace_back<VarRefNode>(&var);
    auto &receiver = _current_module->nodes.emplace_back<AddrOfExprNode>(&var_ref);

    auto &call = _current_module->nodes.emplace_back<FunctionCallExprNode>(
        _current_module->make_virtual_token(name, Token::Type::t_identifier, at),
        std::vector<ExprNode *>{ &receiver });

    // **left unresolved on purpose, and `lookup_namespace` left null** - that is what makes it a member
    // call: CallResolver::candidates_for reads the receiver's type off argument 0, and `$__it` is not
    // typed until the re-derivation sweep later this round. the fixpoint's own settle_calls finishes
    // it, exactly as it finishes every call AST::OperatorRewriter builds
    //
    // this is also the whole of what makes an erased iterator work: find_member_functions finds the
    // requirement in the interface's own `_methods`, and ExprCodegen::gen_function_call routes on
    // FunctionDeclNode::is_interface_requirement(). no arm anywhere
    return call;
}

void ForeachLowering::refuse(
    ScopeNode &scope, size_t index, ForeachNode &loop, const TokenReference &at, std::string why)
{
    _collector.collect_issue<Issue::GenericError>(code_ref_for(at), std::move(why));
    discard(scope, index, loop);
}

void ForeachLowering::discard(ScopeNode &scope, size_t index, ForeachNode &loop)
{
    // the body is dropped rather than kept: `$el` never got a type, so keeping it would cascade a dozen
    // "unknown type" messages underneath the one real diagnostic
    auto &empty = _current_module->nodes.emplace_back<ScopeNode>();
    empty.parent_ptr = &scope;

    scope.children[index] = make_ref(empty);

    loop.source = nullptr;
    loop.key = nullptr;
    loop.element = nullptr;
    loop.body = nullptr;

    _changed = true;
}

void ForeachLowering::lower(ScopeNode &scope, size_t index)
{
    auto *loop = scope.children[index].get_ptr<ForeachNode>();

    if (loop == nullptr || loop->source == nullptr || loop->element == nullptr || loop->body == nullptr) {
        return;
    }

    const IterationLookup look = iteration_plan_for(
        loop->source->result_type(), _collector.core_types, _collector.type_registry);

    if (look.result == IterationLookup::Result::t_pending) {
        // **out of rounds is out of answers** - see finalize(). the discard is the half that matters:
        // a survivor is the InternalCompilerException PointerAdjuster throws, ahead of the gate that
        // would have printed whatever *did* explain the source. the message is only for the case
        // where nothing else did, which has_critical_issues() is already the compiler's answer to
        if (_finalizing) {
            if (_collector.has_critical_issues()) {
                discard(scope, index, *loop);
            }
            else {
                refuse(scope, index, *loop, loop->token_foreach, fmt::format(
                    "'{}' never got a type, so there is nothing to iterate.",
                    loop->source->result_type().get_type_desciption()));
            }
        }

        return;  // ask again next round
    }

    if (look.result == IterationLookup::Result::t_refused) {
        refuse(scope, index, *loop, loop->token_foreach, look.refusal);
        return;
    }

    const IterationPlan &plan = look.plan;

    // the keyed form needs a key contract, and the `=>` is where that belongs - it is the token that
    // asked for one
    if (loop->key != nullptr && !plan.key_type.has_value()) {
        refuse(scope, index, *loop, loop->token_arrow.value_or(loop->token_foreach), fmt::format(
            "'{}' iterates, but its cursor declares no key contract - so there is no '$k' to bind. "
            "Declare 'Keyed<...>' on it, or drop the '=>'.",
            loop->source->result_type().get_type_desciption()));
        return;
    }

    // **a mutable borrow over const elements.** refused here because this is the first moment V is
    // known: the parser cannot know it, and AST::TypeChecker would arrive as a conversion error about a
    // `V&` declaration the author never wrote, with no `&` token left to point at
    if (loop->binding == ForeachNode::Binding::t_borrow && plan.element_type.is_const()) {
        refuse(scope, index, *loop, loop->token_binding.value_or(loop->token_foreach), fmt::format(
            "'{}' asks for a borrow it could write through, but '{}' hands out '{}' elements. "
            "Write 'const &{}' to borrow it read-only, or drop the '&' for a copy.",
            loop->element->name_full(),
            loop->source->result_type().get_type_desciption(),
            plan.element_type.get_type_desciption(),
            loop->element->name_full()));
        return;
    }

    // **the copy elision.** a by-value binding nothing writes is bound `const V&` instead, which is
    // indistinguishable for any program that keeps the "do not mutate while iterating" contract - see
    // book/concept/iteration.md. asked here, exactly once, on a body still shaped as it was parsed
    ForeachNode::Binding effective = loop->binding;

    if (effective == ForeachNode::Binding::t_value && is_never_written(*loop->element, *loop->body)) {
        effective = ForeachNode::Binding::t_const_borrow;
    }

    // a scope of its own, so `$__it` dies at loop exit rather than at the end of whatever block the
    // loop was written in. minted after parsing, so no block token and no lexical namespace - nothing
    // reads ScopeNode::lookup_variable past the parser
    auto &wrapper = _current_module->nodes.emplace_back<ScopeNode>();
    wrapper.parent_ptr = &scope;

    // `$__it`
    auto &iterator_decl = _current_module->nodes.emplace_back<VarDeclNode>(
        _current_module->make_virtual_token("$__it", Token::Type::t_varname, loop->token_foreach),
        nullptr);

    if (plan.kind == IterationSource::t_iterable) {
        // **the callee is set rather than looked up**: AST::iteration_plan_for already chose it through
        // the conformance, and re-resolving by name here would be a second answer to that question - one
        // that could land on a different `iterate()` overload than the plan read V from. that shape - a
        // member call whose callee is already chosen - is AST::make_resolved_member_call's, shared with
        // AST::OwnershipPass, and the receiver's addressing along with it.
        //
        // **the address has to exist now**, which is why this is not left to a later round the way
        // AST::OperatorRewriter leaves an operand bare. `iterate()` on a generic owner is itself
        // generic, so CallResolver::settle returns t_pending at its generic gate and never reaches
        // coerce_arguments - the round that would insert the `&` is the round *after* the one rewiring
        // the call to its instance, and AST::OwnershipPass has walked the body by then, once and for all
        //
        // it is also what gives a temporary source its slot: `foreach ($a->slice(1, 2) as ...)` is an
        // AddrOf over a non-place, which is exactly the shape the ownership pass's addrof arm mints a
        // temporary for
        iterator_decl.init_expr =
            &make_resolved_member_call(*_current_module, plan.iterate, loop->token_foreach, loop->source);

        // **typed here rather than left to the re-derivation sweep.** `plan.iterate` is the *template's*
        // declaration - find_member_functions redirects an instantiation through template_ref - so its
        // return type still reads `slice_iterator<T>` until the monomorphizer rewires the call to the
        // instance. a `$__it` typed from it would then resolve `advance()` against the template and
        // reach codegen with a declaration nothing ever emitted a body for
        //
        // AST::iteration_plan_for already substituted this, which is the whole reason it carries it
        iterator_decl.set_type_node(
            &_current_module->nodes.emplace_back<TypeNode>(plan.iterator_type));
    }
    else if (is_place_expression(*loop->source)) {
        // the source already *is* a cursor, and driving it advances it - so the caller has to see that.
        // a borrow, not a copy
        iterator_decl.init_expr = &_current_module->nodes.emplace_back<AddrOfExprNode>(loop->source);
        iterator_decl.set_type_node(&_current_module->nodes.emplace_back<TypeNode>(
            ValueType::make_pointer(plan.iterator_type, false)));
    }
    else {
        // a cursor produced by a call - `foreach ($a->iterate() as ...)`. `$__it` owns it, and the
        // ownership pass drops it at the wrapper's end through the ordinary frame machinery
        iterator_decl.init_expr = loop->source;
    }

    // used exactly once on every path above, so "one subtree per use" holds by construction
    loop->source = nullptr;

    wrapper.add_vardecl(iterator_decl);

    // `while ($__it->advance())`
    auto &loop_stmt = _current_module->nodes.emplace_back<WhileStatementNode>(
        &iterator_call(iterator_decl, "advance", loop->token_foreach), loop->body);

    wrapper.children.push_back(make_ref(loop_stmt));

    // the two bindings are already `body->children[0..1]` - the parser seeded them - so this fills them
    // in rather than splicing declarations. that is the whole reason they are seeded
    if (loop->key != nullptr) {
        loop->key->init_expr = &iterator_call(
            iterator_decl, "key", loop->token_arrow.value_or(loop->token_foreach));
        loop->key->set_type_node(
            &_current_module->nodes.emplace_back<TypeNode>(plan.key_type.value()));
    }

    ExprNode *element_init = &iterator_call(iterator_decl, "current", loop->element->token_varname);

    ValueType element_type = ValueType::make_mutable(plan.element_type);

    if (effective == ForeachNode::Binding::t_borrow) {
        element_type = ValueType::make_pointer(element_type, false);
    }
    else if (effective == ForeachNode::Binding::t_const_borrow) {
        element_type = ValueType::make_pointer(ValueType::make_const(plan.element_type), false);
    }
    else {
        // **the by-value binding derefs explicitly**, because nothing else will: `current()` hands back
        // `V&` and AST::PointerAdjuster only auto-derefs a *place*, which a call is not - the same
        // limitation that makes `echo $a->at(0)` an error today (todo/A13a). so the read is written
        // here rather than hoped for, and the adjuster leaves it alone: as_value_for over an operand
        // that already answers `V` has nothing left to peel
        //
        // the copy itself is not foreach-specific and is not written here either - OwnershipPass's
        // ordinary declaration arrival inserts the copy constructor when V owns something
        element_init = &_current_module->nodes.emplace_back<DerefExprNode>(element_init);
    }

    loop->element->init_expr = element_init;
    loop->element->set_type_node(&_current_module->nodes.emplace_back<TypeNode>(element_type));

    // `mv` is a contract about a *parameter's* argument, and `$el` is a local
    loop->element->takes_ownership = false;

    scope.children[index] = make_ref(wrapper);

    loop->body = nullptr;
    loop->key = nullptr;
    loop->element = nullptr;

    _changed = true;
}

};
