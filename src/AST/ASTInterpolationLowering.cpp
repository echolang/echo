#include "AST/ASTInterpolationLowering.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTDetach.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTNamespace.h"
#include "AST/ExprNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/StringInterpolationNode.h"

#include <fmt/core.h>

namespace AST
{

InterpolationLowering::InterpolationLowering(Bundle &bundle)
    : FixpointLowering(bundle)
{
}

ExprNode *InterpolationLowering::rewrite_value_edge(ExprNode *expr)
{
    // the descent first, so the holes - and any interpolation inside one - are settled before this
    // literal folds them in
    ExprNode *walked = RecursiveVisitor::rewrite_value_edge(expr);

    if (walked == nullptr || walked->get_node_type() != NodeType::n_string_interpolation) {
        return walked;
    }

    return lower(static_cast<StringInterpolationExprNode &>(*walked));
}

ExprNode &InterpolationLowering::literal_for(
    const StringInterpolationExprNode &node,
    const std::string &bytes,
    const TokenReference *at
)
{
    // **a virtual token carrying the chunk's own text**, located at the literal the author wrote.
    // reusing `token_string` for every chunk would be correct in every way that matters and wrong in
    // the one that is read: `node_description` prints the *token*, so `-p ast-resolved` showed
    // `"n={$n}!"` as two literals both spelled `n=`
    auto &literal = _current_module->nodes.emplace_back<LiteralStringExprNode>(
        _current_module->make_virtual_token(
            bytes, Token::Type::t_string_literal, at != nullptr ? *at : node.token_string));

    literal.decoded_value = bytes;
    literal.core_string_type = node.core_string_type;

    return literal;
}

ExprNode &InterpolationLowering::str_call(
    const std::string &name,
    const TokenReference &at,
    std::vector<ExprNode *> operands
)
{
    auto &call = _current_module->nodes.emplace_back<FunctionCallExprNode>(
        _current_module->make_virtual_token(name, Token::Type::t_identifier, at),
        std::move(operands));

    // **named rather than resolved**, so a user's own `str::from(MyType)` overload wins exactly as it
    // would at a hand written call site - the `hash::of` bargain, and the reason a type joins the
    // formatting surface by declaring one function and nothing else
    call.lookup_namespace = _collector.namespaces.get("str");

    // left unresolved on purpose: the argument may not have settled yet, and the fixpoint's own
    // settle_calls is what finishes every other pending call
    _changed = true;

    return call;
}

ExprNode *InterpolationLowering::refuse(StringInterpolationExprNode &node, std::string why)
{
    _collector.collect_issue<Issue::GenericError>(code_ref_for(node.token_string), std::move(why));

    // the chunks as they were written, holes dropped. a *plain* literal rather than nothing, so the
    // destination this sat in still gets a `string` and the one diagnostic above is the only one
    std::string joined;
    for (const auto &chunk : node.chunks) {
        joined += chunk;
    }

    ExprNode &replacement = literal_for(node, joined);

    // nothing here is kept - the hole expressions go with the node - so the whole subtree is what left
    // the tree, and the arena has to stop answering for it or Monomorphizer::snapshot_calls keeps
    // finding calls inside a literal nobody will emit
    forget_subtree(_bundle, node);
    node.holes.clear();

    _changed = true;
    return &replacement;
}

ExprNode *InterpolationLowering::lower(StringInterpolationExprNode &node)
{
    // **`--no-stdlib` is legitimate**, and there is nothing to lower to there: no `string` to
    // concatenate and no `str` to render through. refused with a sentence naming the standard library
    // rather than reported as three missing functions
    if (!node.core_string_type.has_value() || _collector.namespaces.get("str") == nullptr) {
        return refuse(node,
            "string interpolation needs the standard library's 'string' type and its 'str' namespace. "
            "Write the value out with a single quoted string and 'echo' instead.");
    }

    // the fold. an empty chunk contributes nothing - which is how "two holes with nothing between
    // them" spells itself - so `acc` stays null until the first thing that does
    ExprNode *acc = nullptr;

    auto append = [this, &acc](ExprNode &next, const TokenReference &at) {
        acc = acc == nullptr ? &next : &str_call("concat", at, { acc, &next });
    };

    for (size_t i = 0; i < node.chunks.size(); i++) {
        if (!node.chunks[i].empty()) {
            append(literal_for(node, node.chunks[i]), node.token_string);
        }

        if (i >= node.holes.size()) {
            continue;
        }

        auto &hole = node.holes[i];

        if (hole.expr == nullptr) {
            continue;
        }

        // **one call whether or not a spec was written, and a different arity for each.** `{$x}` asks
        // for the type's own rendering and `{$x:>8}` asks it to honour a spec, which is a question the
        // one-argument overload cannot be handed - so a type that renders but does not format refuses
        // the second spelling as an ordinary "no matching function", naming the overload it is missing
        std::vector<ExprNode *> operands { hole.expr };

        if (hole.spec.has_value()) {
            // located at its own hole rather than at the literal, which is the whole of what a spec
            // literal differs by - a refusal about `{$x:>z}` has to underline that bracket
            operands.push_back(&literal_for(node, hole.spec.value(), &hole.token));
        }

        append(str_call("from", hole.token, std::move(operands)), hole.token);
    }

    // every chunk empty and every hole missing its expression: `""` is the honest answer, and it keeps
    // this total rather than handing a null back up a value edge
    if (acc == nullptr) {
        acc = &literal_for(node, std::string());
    }

    // the holes moved into the calls above, so only the node itself left the tree
    node.holes.clear();
    forget_subtree(_bundle, node);

    _changed = true;
    return acc;
}

};  // namespace AST
