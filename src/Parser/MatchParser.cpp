#include "Parser/MatchParser.h"

#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

#include <fmt/core.h>

namespace
{
// **the owner of a pattern's spelling**, and deliberately not the expression grammar.
//
// `Unit::meter` in a *value* position is the call that builds it, minted by
// Parser::try_parse_static_call - and that is exactly what a pattern must not be. a pattern selects a
// case, so what it needs off these tokens is a name and, when an owner is written, the type that
// declares it; building a call and then taking it apart would mean a case constructor was resolved
// for every arm of every match, against an owner that is not settled yet.
//
// three spellings, one shape: `Owner::name`, `.name` and a bare `name`. the last two say which enum
// they mean only by where they are, which is what AST::MatchResolution answers
struct PatternName
{
    // the owner as written, or nothing for `.name` and a bare `name`
    AST::TypeNode *owner = nullptr;
    std::string name;
};

bool parse_pattern_name(Parser::Payload &payload, PatternName &out)
{
    auto &cursor = payload.cursor;

    // `.name` - the shorthand, whose owner the subject names. the same leading dot
    // Parser::starts_shorthand_call reads, and unambiguous here for a sharper reason than there: a
    // pattern is the head of an arm, so no operand precedes it and `..` cannot begin one
    if (cursor.is_type(Token::Type::t_dot)) {
        cursor.skip();
    }
    else {
        // **the owner is read by the one grammar every `Type::` form is written in**, which is the
        // whole of what this arm has to do.
        //
        // handing `Unit::meter` straight to Parser::parse_type reads it as the nested type
        // `Unit::meter` and reports that `Unit` has no such thing - so the owner's own tokens have to
        // be measured before any of them is parsed, and that measurement is not a scan for a `::`.
        // `Pair<int32, int32>::left` has one inside a type argument list, and the run ends at a `,`
        // that the owner's own spelling contains: telling those apart means counting angle brackets
        // and splitting a `>>`, which is exactly what Parser::try_parse_static_owner already does for
        // `Type::f(...)` and `Type::$x`.
        //
        // it answers null and puts the cursor back for anything that is not an owner - a bare `name`,
        // and an `Owner::` whose owner names no type. the first is a pattern in its own right and the
        // second is read as one, so the missing case is reported against the subject's enum rather
        // than as an unknown type: the same silence the expression grammar keeps here, for the same
        // reason - what these tokens mean is not settled until the subject's type is
        out.owner = Parser::try_parse_static_owner(payload, /*want_property=*/false);
    }

    if (!payload.expect_token(Token::Type::t_identifier)) {
        return false;
    }

    out.name = cursor.current().value();
    cursor.skip();

    return true;
}
}

AST::MatchExprNode *Parser::parse_match(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    const TokenReference match_token = cursor.current();
    cursor.skip(); // `match`

    if (!payload.expect_token(Token::Type::t_open_paren)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    AST::ExprNode *subject_expr = Parser::parse_expr(payload);

    if (subject_expr == nullptr) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    if (!payload.expect_token(Token::Type::t_close_paren)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    // **the scrutinee is a declaration this node owns**, not an expression re-read per arm: the arms
    // read the payload off it, which needs storage, and it has to be evaluated exactly once whatever
    // the arms do. AST::TemporaryBindExprNode's shape, and the name is unspellable for its reason
    //
    // no type node: the initializer's type is what it is, and AST::Monomorphizer's stale-variable
    // sweep derives it exactly as it derives an ordinary `$x = f()` - the arena walk finds this
    // declaration wherever it hangs, so no rule about matches appears in that sweep
    auto subject_token = payload.context.make_virtual_token(
        "$__match", Token::Type::t_varname, match_token);

    // an **unknown** type node rather than none: AST::Monomorphizer's stale-variable sweep is what
    // derives this, and its first guard is `has_type()` - a declaration carrying no type node at all is
    // one nothing has been asked to infer, which is the shape a payload binding wants and this one does
    // not. the same placeholder an ordinary `$x = f()` is parsed with
    auto &subject_type = payload.context.emplace_node<AST::TypeNode>(AST::ValueType::make_unknown());
    auto &subject = payload.context.emplace_node<AST::VarDeclNode>(subject_token, &subject_type);
    subject.init_expr = subject_expr;

    auto &node = payload.context.emplace_node<AST::MatchExprNode>(&subject, match_token);

    if (!payload.expect_token(Token::Type::t_open_brace)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    // **give up on this arm and pick the next one up**, which is what lets one malformed arm cost one
    // diagnostic rather than every arm after it. the trailing `,` goes with the arm being abandoned -
    // unlike the recovery inside a value arm below, which leaves it for the arm-separator step it is
    // still going to reach
    const auto skip_to_next_arm = [&]() {
        cursor.skip_until({ Token::Type::t_comma, Token::Type::t_close_brace });

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        }
    };

    while (!cursor.is_done() && !cursor.is_type(Token::Type::t_close_brace)) {
        AST::MatchExprNode::Arm arm { cursor.current() };

        // the payload bindings, which become the arm scope's own first declarations. built with a null
        // type node - the guard's `failure` shape - because what a binding holds is the case's payload,
        // and which case this is nothing has said yet
        std::vector<AST::VarDeclNode *> bindings;

        // `else => ...` - the catch-all. an existing token rather than a `default` keyword, which is
        // both what Echo already spells "the other branch" with and one fewer word taken out of the
        // identifier space
        if (cursor.is_type(Token::Type::t_else)) {
            cursor.skip();
        }
        else {
            PatternName pattern;

            if (!parse_pattern_name(payload, pattern)) {
                skip_to_next_arm();
                continue;
            }

            // `arm.token` stays the pattern's *first* token, set when the arm was constructed - the
            // `Owner`, the `.` or the bare name. that is where a diagnostic about this arm points, and
            // it is the whole pattern's head rather than the name inside it for the reason
            // FunctionCallExprNode::token_shorthand_dot gives: a reader sent to the name of a `.meter`
            // that could not find its enum would go looking for a typo in the name
            arm.case_name = pattern.name;
            arm.owner = pattern.owner;

            // `(...)` - **bindings, never a selector.** `Unit::meter` and `Unit::meter($v)` choose the
            // same arm; the names only say what to call what is inside. so this reads names and
            // nothing else - no nested pattern, no literal, nothing that could make one arm match a
            // strict subset of another and put the arms in an order that matters
            if (cursor.is_type(Token::Type::t_open_paren)) {
                cursor.skip();

                while (!cursor.is_done() && !cursor.is_type(Token::Type::t_close_paren)) {
                    if (!payload.expect_token(Token::Type::t_varname)) {
                        break;
                    }

                    // **typed `unknown&` rather than left untyped**, and the pointer level is the
                    // load-bearing half. what a binding holds is the case's payload and nothing has
                    // said which case this is - but that it is a *borrow* of one is decided here, and
                    // the parser needs to know it: a member call addresses its receiver unless the
                    // receiver is already an address, and it decides that from the type. left untyped,
                    // `$body->size()` addressed a borrow a second time and read `string&&`
                    //
                    // AST::MatchResolution replaces the pointee once the case is known
                    auto &binding_type = payload.context.emplace_node<AST::TypeNode>(
                        AST::ValueType::make_pointer(AST::ValueType::make_unknown(), false));

                    bindings.push_back(
                        &payload.context.emplace_node<AST::VarDeclNode>(cursor.current(), &binding_type));
                    cursor.skip();

                    if (cursor.is_type(Token::Type::t_comma)) {
                        cursor.skip();
                        continue;
                    }

                    break;
                }

                if (!payload.expect_token(Token::Type::t_close_paren)) {
                    cursor.try_skip_to_next_statement();
                    return nullptr;
                }
                cursor.skip();
            }
        }

        if (!payload.expect_token(Token::Type::t_double_arrow)) {
            skip_to_next_arm();
            continue;
        }
        cursor.skip();

        // **a `{ }` arm and a value arm are one arm shape with one edge unset**, rather than two kinds.
        // the block form produces nothing, so a match holding one is `void` - and that has to be the
        // *match's* answer rather than the arm's, or an arm list could disagree with itself about what
        // the whole form is worth. AST::MatchResolution is where the two are reconciled
        if (cursor.is_type(Token::Type::t_open_brace)) {
            const TokenReference arm_brace = cursor.current();
            cursor.skip();

            arm.scope = &Parser::parse_scope(payload, arm_brace, bindings);

            if (!payload.expect_token(Token::Type::t_close_brace)) {
                cursor.try_skip_to_next_statement();
                return nullptr;
            }
            cursor.skip();
        }
        else {
            // a value arm still gets a scope, and it is not decoration: the bindings have to be
            // resolvable while the value is parsed, and they have to be *somewhere* for the frame
            // machinery to end them. so the scope is opened by hand around the one expression, which
            // is what parse_scope does around a block
            auto &arm_scope = payload.context.emplace_node<AST::ScopeNode>();

            // add_vardecl rather than declare_variable, which is what parse_scope does with its own
            // seeds: a binding is a statement as well as a name, and it is the statement that runs the
            // initializer AST::MatchResolution gives it. registering the name alone leaves a binding
            // that resolves and holds nothing
            for (AST::VarDeclNode *binding : bindings) {
                arm_scope.add_vardecl(*binding);
            }

            payload.context.push_scope(arm_scope);
            arm.value = Parser::parse_expr(payload);
            payload.context.pop_scope();

            arm.scope = &arm_scope;

            if (arm.value == nullptr) {
                cursor.skip_until({ Token::Type::t_comma, Token::Type::t_close_brace });
            }
        }

        node.arms.push_back(arm);

        // the comma separates arms and is optional after a `{ }` one, which reads as a block ending
        // rather than as a list entry - and optional after the last arm either way
        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        }
    }

    if (!payload.expect_token(Token::Type::t_close_brace)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    if (node.arms.empty()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(match_token),
            "A 'match' has to have at least one arm - it is what the value it produces comes from.");
    }

    return &node;
}
