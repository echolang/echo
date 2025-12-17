#include "Parser/ReturnParser.h"

#include "Parser/ExprParser.h"

#include "AST/ASTNullability.h"
#include "AST/TypeNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

AST::ReturnNode &Parser::parse_return(Parser::Payload &payload)
{
    // sanity check that the current token is a return keyword
    if (!payload.cursor.is_type(Token::Type::t_return)) {
        payload.collect_unexpected_token(Token::Type::t_return);
        payload.cursor.try_skip_to_next_statement();
        auto &expr = payload.context.emplace_node<AST::VoidExprNode>();

        return payload.context.emplace_node<AST::ReturnNode>(&expr);
    }

    auto return_token = payload.cursor.current();

    // skip the return keyword
    payload.cursor.skip();

    // a bare `return;` hands back nothing. it has to be recognised before the expression parse
    // rather than after: parse_expr on an empty expression tripped its own
    // `node_stack.size() == 1` assertion, so an early return from a void function crashed the
    // compiler instead of compiling to a `ret void`
    if (payload.cursor.is_type(Token::Type::t_semicolon)) {
        payload.cursor.skip();

        // **inside a constructor it hands back `$this`**, because that is what a constructor
        // returns - the implicit one Parser::parse_typedecl appends when a body writes no return at
        // all is this very node. without this an early `return;` compiled to a `ret void` in a
        // function typed `Foo`, and the ownership pass, seeing a return, unwound the object being
        // built: a destructor call on a half-constructed value, then a garbage result
        //
        // Context::ctor_this_ptr is exactly the right guard - AST::ConstructorScope clears it for
        // every declaration nested in the body, so a `function` or closure written inside a
        // constructor still returns nothing
        if (payload.context.ctor_this_ptr != nullptr) {
            auto *this_var = payload.context.emplace_nodep<AST::VarNode>(payload.context.ctor_this_ptr);
            auto *this_ref = payload.context.emplace_nodep<AST::VarRefNode>(this_var);

            return payload.context.emplace_node<AST::ReturnNode>(this_ref, return_token);
        }

        return payload.context.emplace_node<AST::ReturnNode>(nullptr, return_token);
    }

    // parse the expression that follows the return keyword, typed against the declared return
    // type the same way a variable declaration's initializer is typed against its variable
    //
    // only a concrete primitive is a useful hint though - the literal parsers apply the same rule
    // to themselves, this one keeps the hint from reaching the rest of the expression too
    //
    // **except for a destination that admits absence**, which a `null` in this position genuinely needs:
    // the empty value's *shape* depends on it. an address-like nullable is a null pointer and a wrapped
    // one is a cleared tag, and a null that never learned which it was reached codegen as the former and
    // was then wrapped as if it were present - `return null;` from a `Point?` function answered a
    // `{ i1 true, ptr null }`, which is a value that says it is there and is not
    //
    // it was harmless while `null` could only ever go somewhere pointer-shaped. generalising the flag is
    // what made the destination decide the representation, and this is the site that had to hear about it
    AST::TypeNode *expected_type = payload.context.return_type_ptr;
    if (expected_type != nullptr
        && !AST::can_type_a_literal(expected_type->type)
        && !AST::destination_admits_null(expected_type->type)) {
        expected_type = nullptr;
    }

    auto expr = parse_expr(payload, expected_type);

    // ensure we have a semicolon at the end of the return statement
    if (payload.cursor.is_type(Token::Type::t_semicolon)) {
        payload.cursor.skip();
    }
    // **a failed sub-parse has already reported and already recovered**, so a missing terminator here
    // is a token this statement no longer owns rather than one the author left out.
    // `return f($undeclared);` recovers past its own semicolon inside the call, and reporting anyway
    // named the *next* statement's first token - a reader hunting for a semicolon that is written
    // right there. the skip is still owed either way: a sub-parse that failed without moving leaves
    // the terminator in place, and it is this statement's to consume
    else if (expr != nullptr) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        payload.cursor.try_skip_to_next_statement();
    }

    return payload.context.emplace_node<AST::ReturnNode>(expr, return_token);
}
