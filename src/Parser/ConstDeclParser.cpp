#include "Parser/ConstDeclParser.h"

#include "AST/ASTIssue.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"
#include "AST/ExprNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "Parser/ExprParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/TypeParser.h"

#include <fmt/core.h>

#include <optional>

namespace
{
    // does the initializer name a variable? asked over the *tokens*, from the cursor to the end of the
    // statement, and asked **before** the expression is parsed.
    //
    // before, because the alternative reports twice: the declaration pass parses this initializer against a
    // throwaway scope that holds nothing, so every `$x` in there fails lookup_variable and collects an
    // UnknownVariable of its own - a second diagnostic saying the name does not exist, under the first one
    // explaining that it could not exist here anyway.
    //
    // one token test rather than two rules, which is why `$this` needs no arm: it is a variable like any
    // other, and the answer for it is the same sentence
    std::optional<TokenReference> initializer_names_a_variable(Parser::Cursor &cursor)
    {
        const auto snapshot = cursor.snapshot();
        std::optional<TokenReference> found;

        // a constant's initializer holds no `{` and no `;` of its own - a closure literal is refused just
        // below, and it is the only expression that could carry either - so the statement's semicolon is a
        // reliable end
        while (!cursor.is_done() && !cursor.is_type(Token::Type::t_semicolon)) {
            if (cursor.is_type(Token::Type::t_varname)) {
                // emplace rather than assign: a TokenReference holds a reference to its collection, so it
                // is constructible and not assignable
                found.emplace(cursor.current());
                break;
            }

            cursor.skip();
        }

        cursor.restore(snapshot);

        return found;
    }
};

AST::ConstDeclNode *Parser::parse_constdecl(
    Parser::Payload &payload,
    AST::TypeDeclNode *owner,
    Parser::VisibilityPrefix visibility
)
{
    auto &cursor = payload.cursor;

    assert(cursor.is_type(Token::Type::t_const) && "parse_constdecl called off a 'const'");
    cursor.skip(); // the `const`

    // the written type, when there is one. `const usize MAX = 100;` types its literal here, at the
    // declaration - which is the reason the form exists, since an untyped `const MAX = 100;` hands each use
    // site an `int32` literal to be typed wherever it lands
    AST::TypeNode *type = nullptr;
    if (!constdecl_omits_its_type(cursor)) {
        type = parse_type(payload);
        if (type == nullptr) {
            cursor.try_skip_to_next_statement();
            return nullptr;
        }
    }

    const auto name_token = cursor.current();
    cursor.skip();

    // Parser::starts_constdecl is what routed us here, and it only answers yes on an identifier
    assert(name_token.type() == Token::Type::t_identifier && "a constant's name is a bare identifier");

    if (!cursor.is_type(Token::Type::t_assign)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            fmt::format(
                "The constant '{}' has no value. A constant is the expression it is written as, so there is "
                "nothing to declare without one - write `const {} = <expression>;`",
                name_token.value(), name_token.value()));
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    cursor.skip(); // the `=`

    if (const std::optional<TokenReference> variable = initializer_names_a_variable(cursor)) {
        const TokenReference &variable_token = variable.value();

        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(variable_token),
            fmt::format(
                "A constant's value cannot name a variable. '{}' does not exist where '{}' is used, because "
                "the value is copied to every use site rather than read from here. Write `const {} = ...;` "
                "for a variable that is const instead",
                variable_token.value(), name_token.value(), variable_token.value()));
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // a closure captures its environment where it is *written*, and a constant is copied to where it is
    // *used* - so the two are in direct conflict, and the copies would share one environment besides: the
    // monomorphizer gives a closure *expression* exactly one, which every clone of it then shares
    if (Parser::starts_closure_literal(cursor)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(cursor.current()),
            fmt::format(
                "A constant's value cannot be a closure. A closure captures where it is written and a "
                "constant is copied to where it is used, so '{}' would have no environment to capture",
                name_token.value()));

        // the closure's body goes with it, brace-depth aware - a skip to the next `;` stops at the
        // first one *inside* the body and leaves its closing brace to be reported against whatever
        // follows. the trailing skip is the constant's own terminator, after the body is gone
        Parser::skip_refused_function(payload);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // a constant written in a struct body is reached as `self::MAX` or `buffer::MAX`, through the owner's
    // member surface, so it is already exactly as reachable as its owner - which is what `public` on a
    // member says anyway, and so is accepted here in silence like any other redundant default.
    //
    // a `private` one would be the *member* axis, and this is the one member shape that does not carry it:
    // AST::ConstantExpander is what resolves a constant reference, and it tracks the enclosing type for
    // `self::` but not for a privacy question. refused rather than read as the file axis, which would answer
    // something nobody asked
    if (owner != nullptr && visibility.value == AST::Visibility::t_owner) {
        refuse_visibility_prefix(
            payload,
            visibility,
            "A constant declared inside a type is reached through that type, so it is already as reachable "
            "as its owner is - and there is no narrower scope for one.");

        visibility.value = AST::Visibility::t_public;
    }

    auto &const_node = payload.context.emplace_node<AST::ConstDeclNode>(name_token, type);
    const_node.declared_in = AST::origin_at(payload.context);
    const_node.visibility = visibility.value;
    const_node.value = parse_expr(payload, type);

    if (const_node.value == nullptr) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    if (cursor.is_type(Token::Type::t_semicolon)) {
        cursor.skip();
    }

    // where the name goes: a struct's member surface, or the namespace the file declares into. The member
    // surface is the same namespace a nested type's constructors land in, which is what makes `buffer::MAX`
    // resolve with no expression grammar of its own
    AST::Namespace *target = owner != nullptr
        ? AST::member_surface_namespace(payload.collector.namespaces, owner->complex_type())
        : payload.context.current_namespace;

    if (target == nullptr) {
        return nullptr;
    }

    const_node.owner = owner != nullptr ? &owner->complex_type() : nullptr;

    // **find before create**: Namespace::push_symbol replaces the slot and frees what was there, so
    // publishing over a type symbol would leave that type unresolvable with no diagnostic anywhere
    if (auto *existing = payload.collector.namespaces.find_symbol(name_token.value(), *target)) {
        if (auto *previous_type = existing->node.get_ptr<AST::TypeDeclNode>()) {
            payload.collector.collect_issue<AST::Issue::TypeRedeclaration>(
                payload.context.code_ref(name_token), name_token.value(), previous_type->name_token.value());
            return nullptr;
        }

        if (existing->node.get_ptr<AST::ConstDeclNode>() != nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(name_token),
                fmt::format("The constant '{}' is already declared.", name_token.value()));
            return nullptr;
        }
    }

    target->push_symbol(std::make_unique<AST::Symbol>(&const_node));

    return &const_node;
}
