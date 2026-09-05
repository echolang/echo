#include "Parser/ScopeParser.h"
#include "Parser/MatchParser.h"

#include "AST/VarDeclNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/AssignNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/TypeNode.h"
#include "AST/GuardNode.h"

#include "Parser/VarDeclParser.h"
#include "Parser/EchoPrintParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/FuncCallParser.h"
#include "Parser/GuardParser.h"
#include "Parser/IfStatementParser.h"
#include "Parser/ReturnParser.h"
#include "Parser/WhileStatementParser.h"
#include "Parser/ForStatementParser.h"
#include "Parser/LoopControlParser.h"
#include "Parser/ForeachParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/UseParser.h"
#include "Parser/AttributeParser.h"
#include "Parser/OpaqueDeclParser.h"
#include "Parser/TypeDeclParser.h"
#include "Parser/ExternParser.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/OperatorDeclParser.h"
#include "Parser/TestDeclParser.h"
#include "Parser/VisibilityParser.h"

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

void Parser::finish_place_statement(Parser::Payload &payload, AST::ScopeNode &scope, AST::ExprNode *target)
{
    auto &cursor = payload.cursor;

    // a chain that ended in a call is a statement of its own - `first(&$o)->bump(1);` - and needs no
    // `=`. both kinds, because the chain discovers both: a member call and an indirect one through a
    // callable property
    if (AST::is_call_statement(*target)) {
        finish_call_statement(payload, scope, target);
        return;
    }

    // otherwise the chain named storage, and a statement may only be writing to it
    const TokenReference assign_token = cursor.current();

    if (!cursor.is_type(Token::Type::t_assign)) {
        payload.collect_unexpected_token(Token::Type::t_assign);
        cursor.try_skip_to_next_statement();
        return;
    }

    // **the same tail Parser::parse_varexpr runs** over a `$var`-rooted target: can this be written to,
    // and at what type. the only difference between the two statements is what the chain is rooted in -
    // a call rather than a name - which is not a difference either question acts on
    AST::ExprNode *value = parse_assigned_value(payload, target, assign_token);

    if (value == nullptr) {
        return;
    }

    // no is_initialization arm, and neither shape can want one: a constructor's `$this` is a name, so
    // a call-rooted target is never its field, and `f()[]` never reaches here because an append needs
    // storage the index arm of the postfix chain already refused a call
    scope.children.push_back(AST::make_ref(
        payload.context.emplace_node<AST::AssignNode>(target, value, assign_token)));

    if (!cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        cursor.try_skip_to_next_statement();
        return;
    }

    cursor.skip(); // the semicolon
}

AST::ScopeNode & Parser::parse_scope(
    Parser::Payload &payload,
    std::optional<TokenReference> block_token,
    std::vector<AST::VarDeclNode *> seed_declarations
)
{
    auto &cursor = payload.cursor;
    auto &context = payload.context;

    auto &scope_node = context.emplace_node<AST::ScopeNode>();

    // the opening brace, where this block's DILexicalBlock is placed. the same token the lexical
    // namespace below is keyed on - one block, one position, said once
    if (block_token.has_value()) {
        scope_node.token_brace.emplace(block_token.value());
    }

    // before the first statement, so the names are resolvable while the statements that read them are
    // parsed. in the order given, which is the order they are declared in
    for (AST::VarDeclNode *seed : seed_declarations) {
        if (seed != nullptr) {
            scope_node.add_vardecl(*seed);
        }
    }

    context.push_scope(scope_node);

    // the block's declaration scope. minted for every block rather than only for one that turns out to
    // hold a declaration: a call written *above* the declaration in the same block is stamped with
    // whatever namespace is current when the call is parsed, so a namespace that appeared halfway
    // through the block would leave the earlier call unable to see the later declaration
    AST::LexicalScope lexical_scope(context, payload.collector.namespaces, block_token);

    while (!cursor.is_done()) {
        // **the visibility modifier, read here and at the same point the declaration pass reads it** -
        // one function for both, since the two walks have to consume the same tokens and report the same
        // refusals or they reach different declarations, and there is nothing that would report that. see
        // Parser::parse_declaration_surface for the rest of why this is ahead of the dispatch
        const VisibilityPrefix visibility = consume_declaration_visibility(payload, block_token);

        // deep scope, optionally marked `unsafe`. one arm for both, because an unsafe block *is* a
        // block: it opens a lexical namespace, drops its locals and lowers identically, and the
        // marker changes only what AST::TypeChecker accepts inside it
        if (cursor.is_type(Token::Type::t_open_brace)
            || cursor.is_type_sequence(0, { Token::Type::t_unsafe, Token::Type::t_open_brace })) {
            const bool unsafe_block = cursor.is_type(Token::Type::t_unsafe);
            if (unsafe_block) {
                cursor.skip();
            }

            auto nested_brace = cursor.current();
            cursor.skip();

            AST::ScopeNode &nested = parse_scope(payload, nested_brace);
            nested.is_unsafe = unsafe_block;
            context.scope().add_child_scope(nested);

            // next token needs to be a closing brace
            if (!cursor.is_type(Token::Type::t_close_brace)) {
                payload.collect_unexpected_token(Token::Type::t_close_brace);
                cursor.try_skip_to_next_statement();
                break;
            }

            cursor.skip();
        }
        // this block's own closing brace, left for the caller that opened it - except at the file
        // root, which opened nothing, so a `}` there closes a scope that was never entered
        //
        // reported and skipped rather than treated as the end of the walk, because the declaration
        // pass already walks past it (Parser::parse_declaration_surface's tail skips an unrecognised
        // token) and the two passes must reach the same declarations. breaking here left every
        // statement in the rest of the file unparsed, with no diagnostic and a clean exit status
        else if (cursor.is_type(Token::Type::t_close_brace)) {
            if (block_token.has_value()) {
                break;
            }

            payload.collect_unexpected_token(Token::Type::t_unknown);
            cursor.skip();
        }
        else if (cursor.is_type(Token::Type::t_namespace)) {
            parse_namespacedecl(payload);
        }
        else if (cursor.is_type(Token::Type::t_use)) {
            parse_usedecl(payload, !block_token.has_value());
        }
        else if (starts_funcdecl(cursor)) {
            parse_funcdecl(payload, FuncDeclKind::t_normal, visibility);
        }
        else if (starts_operatordecl(cursor)) {
            // the body. the signature was registered by the declaration pass and the symbol a pass
            // before that, so by here everything about the operator is known except what it does
            parse_operatordecl(payload);
        }
        else if (starts_extern_typedecl(cursor)) {
            parse_opaque_typedecl(payload, visibility.value);
        }
        else if (starts_typedecl(cursor)) {
            parse_typedecl(payload);
        }
        else if (starts_testdecl(cursor)) {
            // the body, and the declaration node with it. the declaration pass refused a test nested in a
            // block already, so this arm parses one either way rather than refusing it twice - the tokens
            // have to be consumed identically by both walks, and a second copy of that sentence would be a
            // diagnostic that appears twice
            parse_testdecl(payload, /*symbol_only=*/false);
        }
        else if (cursor.is_type(Token::Type::t_extern) && !starts_c_function_type(cursor)) {
            parse_extern_block(payload, visibility);
        }
        else if (cursor.is_type(Token::Type::t_return)) {
            scope_node.children.push_back(AST::make_ref(parse_return(payload)));
        }
        // `const if (<condition>) { }` - a branch the compiler decides, whose untaken arm never reaches a
        // later pass. **ahead of the plain `if` arm and of both `const` predicates below**: `const` begins
        // three statements and only the token behind it says which, and starts_vardecl answered yes to a
        // leading `const` whenever the other two declined. it defers to this one now, so the three are a
        // partition rather than an order - but the order is kept anyway, exactly as it is for starts_constdecl
        else if (starts_const_if(cursor)) {
            if (auto *branch = parse_const_ifstatement(payload)) {
                scope_node.children.push_back(AST::make_ref(branch));
            }
        }
        else if (cursor.is_type(Token::Type::t_if)) {
            scope_node.children.push_back(AST::make_ref(parse_ifstatement(payload)));
        }
        // **`guard` is no longer a statement head**, it is an initializer form on an ordinary
        // declaration - `T $x = guard <nullable> else { ... }`. so this arm reports and recovers rather
        // than parsing: the declaration branch below claims the new spelling through starts_vardecl,
        // and a `guard` reaching the head of a statement can only be the old one
        else if (cursor.is_type(Token::Type::t_guard)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cursor.current()),
                "'guard' introduces a declaration's initializer, so the name it binds goes on the left "
                "of the '=' - write 'T $x = guard <value> else { ... }'");
            cursor.try_skip_to_next_statement();
        }
        else if (cursor.is_type(Token::Type::t_while)) {
            scope_node.children.push_back(AST::make_ref(parse_whilestatement(payload)));
        }
        // `for (init; condition; step) { }`. what it hands back is the **wrapper scope** holding the init
        // beside the loop, not the loop - see Parser::parse_forstatement, and AST::ForStatementNode for
        // why the init is a sibling rather than an edge
        else if (cursor.is_type(Token::Type::t_for)) {
            if (auto *wrapper = parse_forstatement(payload)) {
                scope_node.children.push_back(AST::make_ref(wrapper));
            }
        }
        // `foreach ($a as $el) { ... }`. beside `while` for readability; a dedicated keyword token
        // cannot collide with starts_vardecl, so the position is not load-bearing
        else if (cursor.is_type(Token::Type::t_foreach)) {
            if (auto *loop = parse_foreach(payload)) {
                scope_node.children.push_back(AST::make_ref(loop));
            }
        }
        // `match ($u) { ... }` used as a statement, its value discarded. **an arm rather than letting
        // the expression-statement fallthrough take it**, and for one reason: a statement-position
        // match ends at its closing brace and owes no `;`, exactly as `if` and `foreach` do - while
        // every expression statement below is terminated by one. so the two are told apart here, where
        // the keyword is, rather than by a rule about semicolons somewhere further down
        else if (starts_match(cursor)) {
            if (auto *node = parse_match(payload)) {
                scope_node.children.push_back(AST::make_ref(node));
            }

            // and a `;` after it is accepted rather than required: `$x = match (...) { };` puts one
            // there because the *assignment* wants it, and a reader who writes one here is not wrong
            if (cursor.is_type(Token::Type::t_semicolon)) {
                cursor.skip();
            }
        }
        // `break;` / `continue;`. their own token types, so starts_vardecl - which scans the type grammar
        // from an identifier or a type keyword - cannot claim them, and position here is readability only.
        // parse_loop_control hands back null for one written outside a loop, having already reported it
        else if (cursor.is_type(Token::Type::t_break) || cursor.is_type(Token::Type::t_continue)) {
            if (auto *exit_node = parse_loop_control(payload)) {
                scope_node.children.push_back(AST::make_ref(exit_node));
            }
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

        // a compile-time constant, which the *declaration* pass has already read in full - name, value and
        // symbol. skipped here rather than parsed again, and silently: that pass is the one owner of every
        // diagnostic about one, including the refusal of a constant written inside a body, which this arm is
        // also reached for. asked ahead of starts_vardecl, which answers yes to a leading `const` outright
        else if (starts_constdecl(payload)) {
            // the whole statement, not "on to the next terminator": the declaration pass read this one
            // in full, so its shape is known-good, and a value containing a braced group - a closure
            // literal, which that pass refuses but only *after* reading it - has a `;` inside it that
            // is not this statement's
            cursor.skip_statement();
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
            // pointer, `$s->x = ...` writes a member, `$a[$i] = ...` writes an element, and `$i++`
            // desugars to an assignment
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_accessorlr }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_optional_arrow }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_ptr_of }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_open_bracket }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_op_inc }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_op_dec })
        ) {
            // **the one walk that permits a `= guard` initializer.** the four other callers of
            // parse_varexpr read a declaration where no block may follow - a parameter list, a struct
            // body, a `for` header's init and its step - and a guard statement ends in one
            AST::VarDeclNode *var = parse_varexpr(payload, &scope_node, true);

            // **`static` says which type owns storage, and a body has no type to own it.** a local's
            // storage is its frame's, which is the one thing the modifier would be denying - refused
            // here rather than in parse_varexpr, because this is the walk that knows it is reading a
            // body at all. reported *after* the parse, which is what leaves the declaration the local
            // it was written as, and what lets the message name it: the declaration knows both its
            // modifier's token and its name, where the token run ahead of them only had to be scanned
            if (var != nullptr && var->is_static()) {
                payload.collector.collect_issue<AST::Issue::StaticOutsideType>(
                    payload.context.code_ref(var->static_token.value()), var->name_full());
            }
        }

        // neither branch above claims a call through a callable *value*: it is not a declaration, and
        // starts_call_statement is anchored on an identifier
        else if (starts_indirect_call_statement(cursor)) {
            if (auto *call = parse_expr(payload, nullptr)) {
                finish_call_statement(payload, scope_node, call);
            }
        }

        // a statement rooted in a **static property**: `Session::$count = 1;`, `Type::$p->x = 2;`.
        // ahead of the two branches below because both are anchored on an identifier and would consume
        // the prefix as a namespace, leaving a `$name` neither of them accepts
        else if (starts_static_property_statement(payload)) {
            // **the place alone, not parse_expr.** `t_assign` carries a precedence, so a full
            // expression parse swallows the `=` and everything after it - and finish_place_statement
            // then looks for an `=` at the semicolon. the two branches beside this one never noticed
            // because neither of their shapes is ever written to
            if (auto *root = try_parse_static_property(payload)) {
                auto place = parse_postfix_chain(payload, AST::make_ref(*root));

                if (auto *target = place.unsafe_ptr<AST::ExprNode>()) {
                    finish_place_statement(payload, scope_node, target);
                }
            }
            else {
                cursor.try_skip_to_next_statement();
            }
        }

        // a chain rooted in a **constant** rather than in a name or a call. one expression parse,
        // because the root and everything after it is what parse_expr already reads - and then the
        // same tail the call-rooted branch below uses, which is what makes `stdout->write($t);` and
        // `first(&$o)->bump(1);` one statement form with two roots
        else if (starts_constant_chain_statement(payload)) {
            if (auto *root = parse_expr(payload, nullptr)) {
                finish_place_statement(payload, scope_node, root);
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
                // **and whatever hangs off it**, which is the same shape the `$var ->` branch above
                // routes into parse_varexpr: a chain rooted in a call rather than in a name. without
                // this the statement forms lagged the expression form - `echo first(&$o)->x;` read
                // fine while `first(&$o)->bump(1);` was `Unexpected '->'`
                const AST::NodeReference target_ref =
                    Parser::parse_postfix_chain(payload, AST::make_ref(*funccall_node));

                if (!target_ref.has()) {
                    continue;
                }

                finish_place_statement(payload, scope_node, target_ref.unsafe_ptr<AST::ExprNode>());
            }
        }

        else {
            payload.collect_unexpected_token(Token::Type::t_unknown);

            // when we encounter an unexpected token, we skip until we find a semicolon or a brace
            // in the hopes that there is    simply a typo in the code or something minor that we can recover from
            // we might have to skip till the end of the scope otherwise..
            cursor.skip(); // always skip the token causing the issue

            // through the owner of "how far may recovery go", with `{` added to what it stops on: a `}`
            // ends the loop above rather than this scope's contents, and a `{` opens a block this loop
            // can parse. progress is already guaranteed by the skip of the offending token
            cursor.try_skip_to_next_statement({ Token::Type::t_open_brace });
        }
    }

    context.pop_scope();

    return scope_node;
}
