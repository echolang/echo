#include "Parser/ScopeParser.h"

#include "AST/VarDeclNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"

#include "Parser/VarDeclParser.h"
#include "Parser/EchoPrintParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/FuncCallParser.h"
#include "Parser/IfStatementParser.h"
#include "Parser/ReturnParser.h"
#include "Parser/WhileStatementParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/AttributeParser.h"
#include "Parser/TypeDeclParser.h"
#include "Parser/ExternParser.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"

void Parser::finish_call_statement(Parser::Payload &payload, AST::ScopeNode &scope, AST::ExprNode *call)
{
    auto &cursor = payload.cursor;

    // appended before the terminator is checked, so a missing semicolon costs a diagnostic and not
    // the statement - errors accumulate here, they do not abort the parse
    scope.children.push_back(AST::make_ref(call));

    if (!cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        cursor.try_skip_to_next_statement();
        return;
    }

    cursor.skip(); // the semicolon
}

AST::ScopeNode & Parser::parse_scope(
    Parser::Payload &payload, AST::ScopeNode *into, std::optional<TokenReference> block_token)
{
    auto &cursor = payload.cursor;
    auto &context = payload.context;

    auto &scope_node = into ? *into : context.emplace_node<AST::ScopeNode>();

    context.push_scope(scope_node);

    // the block's declaration scope. minted for every block rather than only for one that turns out to
    // hold a declaration: a call written *above* the declaration in the same block is stamped with
    // whatever namespace is current when the call is parsed, so a namespace that appeared halfway
    // through the block would leave the earlier call unable to see the later declaration
    AST::LexicalScope lexical_scope(context, payload.collector.namespaces, block_token);

    while (!cursor.is_done()) {
        // deep scope
        if (cursor.is_type(Token::Type::t_open_brace)) {
            auto nested_brace = cursor.current();
            cursor.skip();
            context.scope().add_child_scope(parse_scope(payload, nullptr, nested_brace));

            // next token needs to be a closing brace
            if (!cursor.is_type(Token::Type::t_close_brace)) {
                payload.collect_unexpected_token(Token::Type::t_close_brace);
                cursor.try_skip_to_next_statement();
                break;
            }

            cursor.skip();
        }
        else if (cursor.is_type(Token::Type::t_close_brace)) {
            break;
        }
        else if (cursor.is_type(Token::Type::t_namespace)) {
            parse_namespacedecl(payload);
        }
        else if (starts_funcdecl(cursor)) {
            parse_funcdecl(payload);
        }
        else if (starts_typedecl(cursor)) {
            parse_typedecl(payload);
        }
        else if (cursor.is_type(Token::Type::t_extern)) {
            parse_extern_block(payload);
        }
        else if (cursor.is_type(Token::Type::t_return)) {
            scope_node.children.push_back(AST::make_ref(parse_return(payload)));
        }
        else if (cursor.is_type(Token::Type::t_if)) {
            scope_node.children.push_back(AST::make_ref(parse_ifstatement(payload)));
        }
        else if (cursor.is_type(Token::Type::t_while)) {
            scope_node.children.push_back(AST::make_ref(parse_whilestatement(payload)));
        }
        // print statement aka "echo $something"
        else if (cursor.is_type(Token::Type::t_echo)) {
            if (auto *echo_node = parse_echo(payload)) { 
                scope_node.children.push_back(AST::make_ref(echo_node));
            }
        }
        // attribute definition
        //   #[attr]
        //   myfunc() {...
        else if (cursor.is_type(Token::Type::t_hash)) {
            parse_attribute(payload);
        }

        // var declaration 
        // can be:
        //   int $foo =
        //   $bar = 
        //   const $ey
        else if (
            starts_vardecl(payload) ||
            // a write through a place rather than into a bare name. these are statements only,
            // so they are not part of what a declaration looks like: `$p:$ = ...` re-seats a
            // pointer, `$s->x = ...` writes a member, and `$i++` desugars to an assignment
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_accessorlr }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_ptr_of }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_op_inc }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_op_dec })
        ) {
            parse_varexpr(payload, &scope_node);
        }

        // neither branch above claims a call through a callable *value*: it is not a declaration, and
        // starts_call_statement is anchored on an identifier
        else if (starts_indirect_call_statement(cursor)) {
            if (auto *call = parse_expr(payload, nullptr)) {
                finish_call_statement(payload, scope_node, call);
            }
        }

        // a call used as a statement. ordered after the vardecl branch above so that
        // `a::b::Foo $foo` still reads as a declaration rather than a qualified call
        else if (starts_call_statement(payload)) {
            // consume a namespace prefix if there is one, so `mem::free($p);` resolves against
            // `mem` rather than the enclosing namespace
            const AST::Namespace *call_namespace = nullptr;
            if (cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
                if (auto *ns_node = parse_namespace(payload)) {
                    call_namespace = ns_node->ast_namespace;
                }
            }

            if (auto *funccall_node = parse_funccall(payload, call_namespace)) {
                finish_call_statement(payload, scope_node, funccall_node);
            }
        }

        else {
            payload.collect_unexpected_token(Token::Type::t_unknown);

            // when we encounter an unexpected token, we skip until we find a semicolon or a brace
            // in the hopes that there is    simply a typo in the code or something minor that we can recover from
            // we might have to skip till the end of the scope otherwise..
            cursor.skip(); // always skip the token causing the issue
            cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_open_brace, Token::Type::t_close_brace });

            // if we find a semicolon or a close brace, we skip it in the hopes that afterwards we can continue parsing
            if (cursor.is_type({ Token::Type::t_semicolon, Token::Type::t_close_brace })) {
                cursor.skip();
            }
        }
    }

    context.pop_scope();

    return scope_node;
}