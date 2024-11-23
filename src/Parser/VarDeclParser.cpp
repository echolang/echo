#include "Parser/VarDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/OperatorNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/TypeNode.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

bool is_vardecl_end_token(const Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_semicolon) || cursor.is_type(Token::Type::t_comma) || cursor.is_type(Token::Type::t_close_paren);
}

// we do not want to actually skip a closing parenthesis
// because the parent parse will check it to ensure it has parsed all arguments
bool should_skip_vardecl_end_token(const Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_semicolon) || cursor.is_type(Token::Type::t_comma);
}

// the left hand side of an assignment statement: the variable itself, or any `->member`,
// `[n]` or `:$` suffix hanging off it. the cursor has to sit right after the varname
static AST::ExprNode *parse_assign_target(Parser::Payload &payload, AST::VarDeclNode *vardecl)
{
    auto var_node = &payload.context.emplace_node<AST::VarNode>(vardecl);
    auto var_ref = &payload.context.emplace_node<AST::VarRefNode>(var_node);
    auto target_ref = Parser::parse_postfix_chain(payload, AST::make_ref(*var_ref));

    if (!target_ref.has()) {
        return nullptr;
    }

    return target_ref.unsafe_ptr<AST::ExprNode>();
}

// `$i++` is a statement, not an expression - it desugars to `$i = $i + 1` here so that every
// arithmetic rule (the element-scaled GEP for pointers, the value coercion, the const check)
// keeps exactly one implementation. the caller has already parsed `operand` once and left the
// cursor on the `++`/`--` token
static AST::ExprNode *build_incdec_value(Parser::Payload &payload, AST::ExprNode *operand, const TokenReference &op_token)
{
    const bool is_increment = op_token.type() == Token::Type::t_op_inc;
    const std::string symbol = is_increment ? "+" : "-";

    // the step is typed from the storage it moves, so `$b++` on an int8 stays an int8 add and
    // `$f++` on a float64 stays in floating point. a pointer operand gets the default int32,
    // which is what the GEP offset wants
    const AST::ValueType operand_type = AST::value_result_type(*operand);
    AST::ExprNode *step = nullptr;

    if (operand_type.is_floating_type()) {
        // a float32 literal is spelled with the trailing `f`, the same shape autocast produces
        const bool single_precision = operand_type.is_primitive_of_type(AST::ValueTypePrimitive::t_float32);
        auto step_token = payload.context.make_virtual_token(single_precision ? "1.0f" : "1.0", Token::Type::t_floating_literal, op_token);
        step = &payload.context.emplace_node<AST::LiteralFloatExprNode>(step_token, operand_type.get_primitive_type());
    }
    else if (operand_type.is_integer_type()) {
        auto step_token = payload.context.make_virtual_token("1", Token::Type::t_integer_literal, op_token);
        step = &payload.context.emplace_node<AST::LiteralIntExprNode>(step_token, operand_type.get_primitive_type());
    }
    else {
        auto step_token = payload.context.make_virtual_token("1", Token::Type::t_integer_literal, op_token);
        step = &payload.context.emplace_node<AST::LiteralIntExprNode>(step_token);
    }

    auto arith_token = payload.context.make_virtual_token(symbol, is_increment ? Token::Type::t_op_add : Token::Type::t_op_sub, op_token);
    auto op = payload.collector.operators.get_operator(symbol);
    auto &op_node = payload.context.emplace_node<AST::OperatorNode>(arith_token, op);

    return &payload.context.emplace_node<AST::BinaryExprNode>(&op_node, operand, step);
}

AST::VarDeclNode *Parser::parse_varexpr(Parser::Payload &payload, AST::ScopeNode *scope)
{
    auto &cursor = payload.cursor;

    AST::TypeNode *type = nullptr;
    AST::VarDeclNode *vardecl = nullptr;
    bool is_const = false;

    // when we have an identifier we assume it to be the variable type
    // the `&` suffix is part of the type grammar now, so parse_type returns the borrow already
    // built and there is nothing to patch up here
    if (can_parse_type(payload))  {
        type = parse_type(payload);
        if (type == nullptr) {
            cursor.try_skip_to_next_statement();
            return nullptr;
        }
        is_const = type->type.is_const();
    }

    // special case is "const" but type must be inferred
    // const $ronon = 10;
    else if (cursor.is_type(Token::Type::t_const)) {
        cursor.skip();
        is_const = true;
    }

    // fetch the varname and skip it
    auto nametoken = cursor.current();
    cursor.skip();

    // ensure that we actually have a varname
    if (nametoken.type() != Token::Type::t_varname) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(nametoken), Token::Type::t_varname, nametoken.type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // check if the name is already taken in the current scope
    AST::VarDeclNode *prev_vardecl = nullptr;
    if (scope != nullptr) {
        prev_vardecl = scope->find_vardecl_by_name(nametoken.value());
    }

    // if the next token is a accessor this is a member reference

    // we have a previous declaration, this might be a mutable variable
    if (prev_vardecl != nullptr) {
        // const is *not* checked here. it used to be, on the declared type's top level, which is
        // the wrong level twice over: `const int& $r` is a mutable borrow of a const pointee, so
        // the guard never fired on the write it should reject, while `const ptr<int> $p` is a const
        // pointer whose pointee may legally be written, so it fired on a write it should allow
        // telling those apart needs the deref AST::PointerAdjuster inserts, so the check now lives
        // in AST::TypeChecker::check_const_target, keyed on the assignment target's shape

        // we do not allow to redefine the type of a variable, the type
        // has to be either explictly set in the firt declaration or inferred
        if (!prev_vardecl->has_type() && type != nullptr) {
            payload.collector.collect_issue<AST::Issue::VariableRedeclaration>(payload.context.code_ref(nametoken), prev_vardecl);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        // an assignment to an existing variable. the left hand side is a place expression:
        // the variable itself, or any `->member` chain hanging off it. both shapes produce the
        // same AssignNode, so codegen resolves them through one lvalue path
        const auto target_start = cursor.snapshot();

        auto *target = parse_assign_target(payload, prev_vardecl);
        if (target == nullptr) {
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        // `$obj->push(5);` reaches here because the statement dispatch routes anything starting
        // `$var ->` to an assignment, and the postfix chain is what discovers the call. it is a
        // statement in its own right, so there is no `=` to demand - the same shape the call
        // statement branch in ScopeParser handles for a free function
        if (target->get_node_type() == AST::NodeType::n_expr_call) {
            finish_call_statement(payload, payload.context.scope(), target);
            return nullptr;
        }

        AST::ExprNode *expr = nullptr;
        TokenReference assign_token = cursor.current();

        // `$i++` / `$p:$--`. the statement carries no `=`, the step comes from the operator
        if (cursor.is_type(Token::Type::t_op_inc) || cursor.is_type(Token::Type::t_op_dec)) {
            // the operand of the arithmetic is the target parsed a *second* time rather than
            // the same node under two parents: AST::PointerAdjuster rewrites edges in place,
            // and a shared subtree would be adjusted twice - an index expression would collect
            // a second deref on the way through
            cursor.restore(target_start);
            auto *operand = parse_assign_target(payload, prev_vardecl);
            cursor.skip(); // the ++/-- token, re-reached by the second parse

            // the same destination rule the `=` path applies - `$p:$` is legal, `$p:$:$++` is not
            if (operand == nullptr || !AST::is_assignable_target(*target)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(assign_token),
                    "'" + assign_token.value() + "' needs an expression with storage to step");
                cursor.try_skip_to_next_statement();
                return nullptr;
            }

            expr = build_incdec_value(payload, operand, assign_token);
        }

        else {
            if (!payload.cursor.is_type(Token::Type::t_assign)) {
                payload.collect_unexpected_token(Token::Type::t_assign);
                cursor.try_skip_to_next_statement();
                return nullptr;
            }

            // reported here, on the `=`, rather than left to the type checker or codegen. an
            // address has no storage behind it, so `$p:$:$ = &$q` reached the lvalue codegen's
            // "not addressable" throw with no location at all
            if (!AST::is_assignable_target(*target)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(assign_token),
                    "cannot assign to this expression - it has no storage to write into");
                cursor.try_skip_to_next_statement();
                return nullptr;
            }

            cursor.skip();

            // the value is expected at the type the *storage* holds. for a pointer target that is
            // the pointee, because assigning to a pointer writes through it - `$p = 20` never
            // changes where $p points (book/concept/pointers_and_refs_v2.md, "Binding, writing,
            // and re-seating"). a declaration is the other case and binds instead, which is why
            // the init_expr path below keeps the full declared type
            auto &expected = payload.context.emplace_node<AST::TypeNode>(
                AST::value_result_type(*target));

            expr = parse_expr(payload, &expected);
        }

        auto assign = &payload.context.emplace_node<AST::AssignNode>(target, expr, assign_token);

        // a constructor writing a field of its own `$this` binds that field for the first time. the
        // slot is fresh - gen_var_decl zero-fills it - so there is no previous value owed a teardown,
        // and a `const` property gets its one legitimate write. exactly what the synthesized
        // field-wise constructor already says about its own writes, said here so the two agree
        //
        // decided in the parser because this is where knowing it is free: Context::ctor_this_ptr is
        // the enclosing constructor's `$this`, and a later pass would have to reconstruct "are we
        // inside a constructor, and is this that constructor's receiver" from the tree
        if (payload.context.ctor_this_ptr != nullptr
            && target->get_node_type() == AST::NodeType::n_member_access
            && AST::place_root_of(target) == payload.context.ctor_this_ptr) {
            assign->is_initialization = true;
        }

        // skip the end of the statement
        if (is_vardecl_end_token(cursor)) {
            if (should_skip_vardecl_end_token(cursor)) {
                cursor.skip();
            }
        }

        payload.context.scope().children.push_back(AST::make_ref(assign));

        return nullptr;
    }

    vardecl = &payload.context.emplace_node<AST::VarDeclNode>(nametoken, type);

    // if we have a scope we add the variable to it
    if (scope != nullptr) {
        scope->add_vardecl(*vardecl);
    }

    // if next token is a semicolon or comma we are done for now
    if (is_vardecl_end_token(cursor)) {
        if (should_skip_vardecl_end_token(cursor)) {
            cursor.skip();
        }
        return vardecl;
    }

    if (!payload.cursor.is_type(Token::Type::t_assign)) {
        payload.collect_unexpected_token(Token::Type::t_assign);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    cursor.skip();

    // parse the expression
    auto expr = parse_expr(payload, vardecl->optional_type_node());
    vardecl->init_expr = expr;

    if (!vardecl->has_type()) {
        // if there is no explicit type we need to be able to infer it
        if (vardecl->init_expr == nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(payload.context.code_ref(cursor.current()), "cannot infer type of variable without an initializer");
            cursor.try_skip_to_next_statement();
            return nullptr;
        }
        else {
            // the inferred type is the single source of truth, const included - there is no
            // longer a separate node-level flag that could disagree with it
            // value_result_type, not result_type: `$copy = $r` over an `int32&` copies the int
            // it refers to, so the copy is an int32 rather than a second reference
            auto inferred = AST::value_result_type(*vardecl->init_expr);
            vardecl->set_type_node(&payload.context.emplace_node<AST::TypeNode>(
                is_const ? AST::ValueType::make_const(inferred) : inferred));
        }
    }

    // skip the end of the statement
    if (is_vardecl_end_token(cursor)) {
        if (should_skip_vardecl_end_token(cursor)) {
            cursor.skip();
        }
    }

    return vardecl;
}