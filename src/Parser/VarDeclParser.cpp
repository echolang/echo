#include "Parser/VarDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/GuardNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/OperatorNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/TypeNode.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/GuardParser.h"
#include "Parser/ScopeParser.h"

#include <fmt/core.h>

// a closing bracket is here for the same reason a closing parenthesis is: an index operator's
// `[usize $i]` is an ordinary parameter list that happens to be enclosed differently, and it reaches
// this function through the very same Parser::parse_parameter_list
bool is_vardecl_end_token(const Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_semicolon)
        || cursor.is_type(Token::Type::t_comma)
        || cursor.is_type(Token::Type::t_close_paren)
        || cursor.is_type(Token::Type::t_close_bracket);
}

// we do not want to actually skip a closing parenthesis or bracket
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

AST::ExprNode *Parser::parse_assigned_value(
    Parser::Payload &payload,
    AST::ExprNode *target,
    const TokenReference &assign_token
)
{
    auto &cursor = payload.cursor;

    // reported here, on the `=`, rather than left to the type checker or codegen. an address has no
    // storage behind it, so `$p:$:$ = &$q` reached the lvalue codegen's "not addressable" throw with
    // no location at all
    if (!AST::is_assignable_target(*target)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(assign_token),
            "cannot assign to this expression - it has no storage to write into");
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    cursor.skip(); // the '='

    // **`guard` declares, it never assigns.** writing into a name that already exists would owe the
    // value it holds an end on the path that binds and leave it alone on the path that leaves, which
    // are two different programs - and the binding's whole meaning is that it certainly has a value
    // from here on, which a name declared above the guard cannot promise
    //
    // one site for all four assignment shapes, because this function is the sole owner of "the value an
    // assignment writes into `target`": `$x =`, `$s->x =`, `$a[$i] =` and `Type::$p =` all pass here
    if (cursor.is_type(Token::Type::t_guard)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(cursor.current()),
            "'guard' can only introduce a new declaration, and this name already holds storage. "
            "Writing to it would have to end the value it holds on the path that binds and leave it "
            "alone on the path that leaves. Declare a new name instead.");
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // the value is expected at the type the *storage* holds. for a pointer target that is the pointee,
    // because assigning to a pointer writes through it - `$p = 20` never changes where $p points
    auto &expected = payload.context.emplace_node<AST::TypeNode>(AST::value_result_type(*target));

    AST::ExprNode *value = parse_expr(payload, &expected);

    if (value == nullptr) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    return value;
}

AST::VarDeclNode *Parser::parse_varexpr(
    Parser::Payload &payload,
    AST::ScopeNode *scope,
    bool allows_guard
)
{
    auto &cursor = payload.cursor;

    AST::TypeNode *type = nullptr;
    AST::VarDeclNode *vardecl = nullptr;
    bool is_const = false;

    // **`static` is storage the type owns rather than storage each value carries**, and that is the
    // whole of what it changes about the declaration read below: the same type grammar, the same name,
    // the same initializer. where the storage comes from is Compiler::LLVM::StaticStorageCodegen's
    // question, and whether a `static` is legal *here* is the caller's - a local is refused by the
    // statement dispatch, which is the only place that knows it was reading a body
    std::optional<TokenReference> static_token;

    if (cursor.is_type(Token::Type::t_static)) {
        static_token.emplace(cursor.current());
        cursor.skip();
    }

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

    // check if the name is already taken in the current scope. a hit *past* a function boundary does
    // not count: `int32 $x = 2;` written inside a nested function body over an enclosing `$x` declares a
    // fresh variable that shadows it, and treating it as an assignment would have the nested body write
    // into a frame it cannot even address
    AST::VarDeclNode *prev_vardecl = nullptr;
    if (scope != nullptr) {
        const auto found = scope->lookup_variable(nametoken.value());
        prev_vardecl = found.found_in_frame() ? found.decl : nullptr;
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

        // an assignment target binds its storage rather than reading it, which is what makes
        // `$a[] = 5` legal where `echo $a[]` is not. see AST::IndexExprNode::slot_is_bound, whose
        // other setter is the `&` arm of Parser::parse_postfix_chain's caller
        if (target->get_node_type() == AST::NodeType::n_expr_index) {
            auto *index = static_cast<AST::IndexExprNode *>(target);

            index->slot_is_bound = true;

            // and the narrower fact, which only this setter may record: there is an `=` behind this
            // bracket. see AST::IndexExprNode::is_assignment_target for what turns on it
            index->is_assignment_target = true;
        }

        // `$obj->push(5);` reaches here because the statement dispatch routes anything starting
        // `$var ->` to an assignment, and the postfix chain is what discovers the call. it is a
        // statement in its own right, so there is no `=` to demand - the same shape the call
        // statement branch in ScopeParser handles for a free function
        //
        // both kinds of call, because the chain discovers both: `$obj->push(5)` is a member call and
        // `$obj->op(5)` over a callable *property* is an indirect one, and which of the two a name is
        // decided in parse_postfix_chain, not here
        if (AST::is_call_expression(*target)) {
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

            // a declaration is the other case and binds instead, which is why the init_expr path
            // below keeps the full declared type rather than the storage's
            expr = parse_assigned_value(payload, target, assign_token);

            if (expr == nullptr) {
                return nullptr;
            }
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

        // **an append writes a slot that has just been grown into existence**, so there is no old
        // value and none is owed a teardown - running one would destroy whatever bytes the buffer
        // happened to hold. the same question the constructor case above asks, "is this storage
        // fresh", asked of the other way a program can produce fresh storage
        if (target->get_node_type() == AST::NodeType::n_expr_index
            && static_cast<AST::IndexExprNode *>(target)->is_append()) {
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

    // emplace rather than assign: TokenReference has no copy assignment, which is why every optional
    // of one in the tree is filled this way
    if (static_token.has_value()) {
        vardecl->static_token.emplace(static_token.value());
    }

    // **`= guard` is a declaration whose initializer runs inside a branch**, and that changes exactly
    // two things about the ordinary declaration read below: how the name is registered, and who parses
    // the initializer. two tokens of look-ahead rather than a snapshot, because the answer is entirely
    // local - nothing before the `=` differs
    const bool guard_initializer = allows_guard
        && cursor.is_type_sequence(0, { Token::Type::t_assign, Token::Type::t_guard });

    // if we have a scope we add the variable to it
    if (scope != nullptr) {
        // **the name only, for a guard.** the declaration is the guard statement's own - its
        // initializer runs once, inside the branch that found a value - so appending it here as well
        // would emit that initializer a second time as an ordinary statement. that is what
        // AST::ScopeNode::declare_variable exists for, and the bug it was minted to fix was a leaked
        // retain from exactly this double emit
        //
        // a fork rather than an add-then-take-back: the child list *is* the statement order, and an
        // invariant that only holds between two lines nobody reads together is not one
        if (guard_initializer) {
            scope->declare_variable(*vardecl);
        }
        else {
            scope->add_vardecl(*vardecl);
        }
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

    // **the guard form, and the initializer is deliberately parsed with no expected type.** that falls
    // out of taking this branch ahead of the parse_expr below rather than being a rule stated in a
    // comment: the declared type of a guard binding is the *unwrapped* one - `Node $n = guard <Node?>`
    // - so handing `vardecl->optional_type_node()` down as the expectation would bind a written `null`
    // to the wrong shape and tell `&$obj` to produce a borrow where a weak was meant
    //
    // Parser::parse_guard owns everything from the `guard` keyword to the else arm's `}` and appends
    // the statement itself; the declaration is handed back the way any other is, so the caller that
    // knows it is reading a body can still refuse a `static` on it
    if (guard_initializer) {
        AST::GuardNode *guard = parse_guard(payload, *vardecl, is_const);

        // appended to the same scope the *name* went into above, so the two halves of the statement
        // cannot end up in different blocks. `scope` is non-null here by construction:
        // `guard_initializer` is only true for the one caller that reads a body
        if (guard != nullptr && scope != nullptr) {
            scope->children.push_back(AST::make_ref(*guard));
        }

        return vardecl;
    }

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
            // AST::infer_declaration_type owns the rule - see it for both halves and for the
            // other two askers, the monomorphizer's re-derivation sweep and the array literal.
            //
            // an array literal answers *unknown* here and is meant to: the elements say what goes in
            // and never what holds them, so the type arrives in the fixpoint, from
            // AST::array_literal_type_for - the same state `$x = f();` on an unsettled call is in
            vardecl->set_type_node(&payload.context.emplace_node<AST::TypeNode>(
                AST::infer_declaration_type(*vardecl->init_expr, is_const)));
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
