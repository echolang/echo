#include "AST/ASTOperatorSemantics.h"

#include "AST/ASTCollector.h"
#include "AST/ASTCompleteness.h"
#include "AST/ASTModule.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTNullability.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

namespace AST
{
    std::string operator_function_name(const std::string &spelling, OpFixity fixity)
    {
        // **only the unary fixities carry a word**, and only they need one: a prefix and a suffix
        // declaration of one symbol take one parameter of the same type, so they are otherwise the
        // same signature and would clash as a DuplicateFunctionSignature
        //
        // infix needs none - it separates from both on arity, which AST::match_function compares
        // before it looks at any type - and neither does the index form, whose spelling can only ever
        // be an index operator, since `[` and `]` are refused inside every other symbol
        //
        // **the index *write* form is the one that carries punctuation rather than a word**, and it has
        // to carry something: arity cannot separate it from the borrowing form, because
        // `operator (M&)[K, K] : V&` and `operator (M&)[K] = (V) : void` are both three operands. the
        // `=` is available for it exactly because a bracket spelling can never hold one
        switch (fixity) {
            case OpFixity::t_prefix:
                return "operator prefix " + spelling;
            case OpFixity::t_suffix:
                return "operator suffix " + spelling;
            case OpFixity::t_index_write:
                return "operator " + spelling + "=";
            case OpFixity::t_infix:
            case OpFixity::t_index:
                break;
        }

        return "operator " + spelling;
    }

    const std::string &index_operator_name()
    {
        // computed once. the inputs are two constants, and both askers want the same string rather than
        // a fresh one - so the answer outlives the call the way OperatorRegistry::bracket_spelling's does
        static const std::string name =
            operator_function_name(OperatorRegistry::bracket_spelling(), OpFixity::t_index);

        return name;
    }

    const std::string &index_write_operator_name()
    {
        static const std::string name =
            operator_function_name(OperatorRegistry::bracket_spelling(), OpFixity::t_index_write);

        return name;
    }

    ValueType operator_receiver_type(const FunctionDeclNode &decl)
    {
        if (decl.args.empty()) {
            return ValueType::make_unknown();
        }

        // the operand as a *value*, which is what `map<K, V>& $m` names once the borrow is read through.
        // one level only, which is all a receiver ever has - AST::target_type_of's deeper walk is for a
        // `->` chain, and an operand list has no chain
        return value_type_of(decl.parameter_type(0));
    }

    bool declares_index_write(Collector &collector, const ValueType &receiver, size_t index_count)
    {
        if (!receiver.has_complex_type()) {
            return false;
        }

        const ComplexType *wanted = receiver.get_complex_type()->template_or_self();

        // the receiver, then every index, then the value
        const size_t arity = index_count + 2;

        for (const FunctionDeclNode *candidate :
                collector.functions.overloads(index_write_operator_name(), collector.namespaces.root())) {
            if (candidate == nullptr || candidate->args.size() != arity) {
                continue;
            }

            const ValueType declared = operator_receiver_type(*candidate);

            // constness and the borrow level are deliberately not part of this: "which type" is the
            // question, and whether *this* receiver may reach *that* declaration is argument_fit's -
            // asked later, of a candidate set this only decides the shape of
            if (declared.has_complex_type() && declared.get_complex_type()->template_or_self() == wanted) {
                return true;
            }
        }

        return false;
    }

    FunctionCallExprNode &build_operator_call_node(
        Module &module,
        Collector &collector,
        const std::string &spelling,
        OpFixity fixity,
        const TokenReference &at,
        std::vector<ExprNode *> operands
    )
    {
        // the overload set's key, derived from the symbol and the position it was consumed in - the
        // caller names that position once, in the gate that decided to come here
        const std::string decorated_name = operator_function_name(spelling, fixity);

        // the name is **virtual**, because no token in the source spells it. typed t_identifier so
        // AST::is_print_call - which keys on t_echo - cannot mistake it for `echo`
        const TokenReference name_token =
            module.make_virtual_token(decorated_name, Token::Type::t_identifier, at);

        auto &call = module.nodes.emplace_back<FunctionCallExprNode>(name_token, std::move(operands));

        // the root namespace, for the reason the header gives
        call.lookup_namespace = &collector.namespaces.root();

        return call;
    }

    std::string mangle_operator_name(const std::string &decorated_name)
    {
        std::string mangled;
        mangled.reserve(decorated_name.size());

        for (const unsigned char c : decorated_name) {
            const bool safe = (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || c == '_';

            if (safe) {
                mangled.push_back(static_cast<char>(c));
            } else {
                mangled += fmt::format("x{:02x}", c);
            }
        }

        return mangled;
    }

    OperandFacts parse_time_operand(const ExprNode *expr, const ValueType &result_type)
    {
        if (expr == nullptr) {
            return {};
        }

        return OperandFacts{value_result_type(*expr, result_type), is_written_null(expr)};
    }

    OperandFacts parse_time_operand(const ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        return parse_time_operand(expr, expr->result_type());
    }

    OperandFacts adjusted_operand(const ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        return OperandFacts{expr->result_type(), is_written_null(expr)};
    }

    bool simd_lane_op_lowers(Token::Type op, const ValueType &lane)
    {
        const ValueType bare = ValueType::make_mutable(lane);

        // integer lanes: the arithmetic and bitwise a scalar integer has, except `/` `%` `**`,
        // which stay builtin so a declared operator cannot claim them and are refused rather
        // than lowered. a shift's count is not a lane; the caller checks that separately
        if (bare.is_integer_type()) {
            switch (op) {
                case Token::Type::t_op_add:
                case Token::Type::t_op_sub:
                case Token::Type::t_op_mul:
                case Token::Type::t_and:
                case Token::Type::t_or:
                case Token::Type::t_xor:
                case Token::Type::t_op_shl:
                case Token::Type::t_op_shr:
                case Token::Type::t_tilde:
                    return true;
                default:
                    return Operator::is_comparison_token(op);
            }
        }

        // float lanes: `+ - * /` and the six comparisons. no bitwise, no shift, no `**`
        if (bare.is_floating_type()) {
            switch (op) {
                case Token::Type::t_op_add:
                case Token::Type::t_op_sub:
                case Token::Type::t_op_mul:
                case Token::Type::t_op_div:
                    return true;
                default:
                    return Operator::is_comparison_token(op);
            }
        }

        // bool lanes: a mask, not a number. `& | ^` and `==` `!=`. ordering is a refusal
        if (bare.is_boolean_type()) {
            return op == Token::Type::t_and
                || op == Token::Type::t_or
                || op == Token::Type::t_xor
                || op == Token::Type::t_exclamation
                || Operator::is_identity_comparison_token(op);
        }

        return false;
    }

    bool binary_has_builtin_meaning(
        const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs)
    {
        if (op == nullptr) {
            return false;
        }

        // the language spells no meaning for a declared symbol, so there is nothing to fall through
        // to and nothing to weigh a declaration against
        if (op->is_custom()) {
            return false;
        }

        // **the arms below are ExprCodegen::gen_binary_expr's, in its order.** that mirroring is the
        // whole contract: this function's `true` *is* the claim that that one has an arm. ending in
        // "neither operand is a struct, a class or an interface" would promise an arm for every
        // operator over every other pair. it has far fewer, so `1 << 2`, `$a == $b` over two bools, and a
        // weak against null would type-check and then reach a throw with no location at all. an arm
        // added there wants one added here, and the reverse
        //
        // **not decided yet, so nothing is claimed either way.** an unresolved call's result type is
        // unknown, and inside `function add<T>(T $a, T $b)` the operands are a bare `T` - the answer has
        // to be *true* there, or Parser::parse_binary_expr builds an operator call out of a template body
        // whose `T` nothing has bound. AST::OperatorRewriter re-asks once the round substituted it, which
        // is what makes that pass self-guarding: the only nodes it rewrites are ones the parser could not
        // have. a written `null` is excluded because its own type is unknown and the two arms below are
        // the ones that answer for it
        const bool lhs_pending = !lhs.is_null && is_undetermined_type(lhs.type);
        const bool rhs_pending = !rhs.is_null && is_undetermined_type(rhs.type);

        if (lhs_pending || rhs_pending) {
            return true;
        }

        // **anything nullable against `null`**: not "are these two the same object" but "is this one
        // there at all". the language lowers it for every shape - an address comparison where the type
        // has a null value of its own, a tag test where it does not - so it has a built-in meaning even
        // over a struct, where `==` otherwise has none. one predicate for all four shapes, the one
        // TypeLowering::gen_has_value answers for, so a weak needs no arm of its own here
        //
        // one side has to be a written `null`. `$a == $b` over two `Point?`s is a question about the
        // *values*, which is exactly what a declared `==` on Point would be for, so it is left to fall
        // through to the declaration the way it always did
        if (op->is_identity_comparison()
            && ((lhs.is_null && destination_admits_null(rhs.type))
                || (rhs.is_null && destination_admits_null(lhs.type)))) {
            return true;
        }

        // two class handles - the only operators a class answers, and how two references are told
        // apart. a written null against a class that may be absent is the presence_test arm above;
        // a non-nullable `Foo` against null has no builtin meaning, so the parser builds an operator
        // call and AST::binary_operand_refusal is the sentence
        if (lhs.type.is_class() || rhs.type.is_class()) {
            return op->is_identity_comparison()
                && lhs.type.is_class()
                && rhs.type.is_class();
        }

        // **a written null against something that cannot be absent.** deliberately still *true* for a
        // primitive and *false* for a named type, which is the answer the tail gives and which
        // both goldens rest on: `$i == null` is the built-in comparison and AST::binary_operand_refusal
        // explains why it cannot work, one message; `$p == null` on a struct becomes an operator call,
        // because a declared `==` is a thing an author could reach for there
        if (lhs.is_null || rhs.is_null) {
            return !(lhs.is_null ? rhs : lhs).type.has_complex_type();
        }

        // a weak handle answers *presence*, above, and nothing else: comparing two of them asks whether
        // the same reference was taken, which is a question about a handle rather than about an object,
        // and `strong($w)` is how the object is asked after
        if (lhs.type.is_weak() || rhs.type.is_weak()) {
            return false;
        }

        // an address, reached through `:$` - the six comparisons, and offsetting by an element count.
        // two addresses subtract to a distance and cannot be added
        if (lhs.type.is_pointer() || rhs.type.is_pointer()) {
            if (op->is_comparison() || op->type == Token::Type::t_op_sub) {
                return true;
            }

            return op->type == Token::Type::t_op_add
                && !(lhs.type.is_pointer() && rhs.type.is_pointer());
        }

        // **a vector, elementwise.** after the pointer arm and before the integer one, matching
        // ExprCodegen::gen_binary_expr. a simd is not an integer type, so without this arm every
        // vector operator would look for a declared `operator` and never reach a lowering.
        //
        // every symbol but `&&` / `||` is the language's, including the ones that do not lower.
        // those stay *true* so a declared `operator /` cannot claim an integer vector, and so a
        // float-vector shift does not go hunting for a declaration either.
        // AST::simd_lane_op_lowers is the matrix; AST::binary_operand_refusal is the sentence.
        // `&&` / `||` stay scalar-bool: gen_binary_expr's short-circuit lowering is type-blind,
        // so a true here would CreateCondBr on a mask
        if (lhs.type.is_simd() || rhs.type.is_simd()) {
            return op->type != Token::Type::t_logical_and
                && op->type != Token::Type::t_logical_or;
        }

        // **two payload-free enums compare by their discriminant, and that is a built-in meaning rather
        // than a declared one** - an enum's identity *is* which case it is holding, so there is nothing
        // for an author to define and nothing a library could get wrong.
        //
        // **payload-free only**, and the exclusion is the content: over a case that carries something,
        // `E::text("a") == E::text("b")` would be true on the tags alone, which is a wrong answer rather
        // than a missing one. so an enum with any payload falls through to the declaration path, where
        // an author who wants an equality can write the one they mean - and where, until they do, the
        // ordinary "not supported on these operands" refusal points them at `match`
        if (lhs.type.is_enum() || rhs.type.is_enum()) {
            // **compared with the `const` stripped**, which is what makes `$this == Side::left` inside a
            // `const function` work: a const receiver reads as `const Side` and the case it names as
            // `Side`, and whether a value may be written has nothing to say about which case it holds
            return op->is_identity_comparison()
                && ValueType::make_mutable(lhs.type) == ValueType::make_mutable(rhs.type)
                && lhs.type.is_enum()
                && !lhs.type.get_complex_type()->has_payload_case();
        }

        // **a value that may be absent has no arithmetic**, and this is the one arm here that fixes a
        // wrong answer rather than a missing one: `is_integer_type()` is true for an `int32?`, so two of
        // them reached the integer arm below and codegen compared the `{ i1, i32 }` pairs as numbers.
        // the only question a wrapped optional answers is presence, which the two arms above took
        if (lhs.type.is_wrapped_optional() || rhs.type.is_wrapped_optional()) {
            return false;
        }

        if (lhs.type.is_integer_type() && rhs.type.is_integer_type()) {
            switch (op->type) {
                case Token::Type::t_op_add:
                case Token::Type::t_op_sub:
                case Token::Type::t_op_mul:
                case Token::Type::t_op_div:
                case Token::Type::t_op_mod:
                case Token::Type::t_op_pow:
                // **the bitwise five, and they are integer-only by construction.** each sits in the
                // predefined set and carries a tier in AST::op_precedence following C's ordering - shift
                // tighter than `&`, `&` tighter than `^`, `^` tighter than `|` - so nothing here decides
                // how they group. The arm below is what makes them mean anything: a float, a bool and a
                // pointer all fall through to `is_comparison()` and are refused, which is right for every
                // one of them. `>>` reads the operation's signedness in codegen, so it is arithmetic over
                // a signed operand and logical over an unsigned one
                case Token::Type::t_and:
                case Token::Type::t_or:
                case Token::Type::t_xor:
                case Token::Type::t_op_shl:
                case Token::Type::t_op_shr:
                    return true;
                default:
                    return op->is_comparison();
            }
        }

        // a bool is a yes/no rather than a small number: `&&`/`||` combine two, `==`/`!=` ask whether
        // they agree, and ordering one against another is refused the way ordering two class handles is
        if (lhs.type.is_boolean_type() && rhs.type.is_boolean_type()) {
            return op->type == Token::Type::t_logical_and
                || op->type == Token::Type::t_logical_or
                || op->is_identity_comparison();
        }

        // a float meeting another number, which codegen promotes both sides of. no `**` - that arm
        // round-trips through `llvm.pow` for integers only - and no `^`
        const auto numeric = [](const ValueType &type) {
            return type.is_numeric_type() || type.is_boolean_type();
        };

        if ((lhs.type.is_floating_type() || rhs.type.is_floating_type())
            && numeric(lhs.type) && numeric(rhs.type)) {
            switch (op->type) {
                case Token::Type::t_op_add:
                case Token::Type::t_op_sub:
                case Token::Type::t_op_mul:
                case Token::Type::t_op_div:
                case Token::Type::t_op_mod:
                    return true;
                default:
                    return op->is_comparison();
            }
        }

        return false;
    }

    bool unary_has_builtin_meaning(const Operator *op, const OperandFacts &operand)
    {
        if (op == nullptr || op->is_custom()) {
            return false;
        }

        // a vector asks the lane matrix, one place. a lane that is still a type parameter has
        // not been decided, for the same reason a bare `T` is admitted below
        if (operand.type.is_simd()) {
            if (is_undetermined_type(operand.type)) {
                return true;
            }

            return simd_lane_op_lowers(
                op->type, ValueType::make_mutable(operand.type.simd_element()));
        }

        // negation over a number, which is the whole of gen_unary_expr - plus unary `+`, which the
        // parser folds away because it carries no semantics. that fold *is* a built-in meaning, and
        // saying so here is what keeps `+5` an int rather than a lookup for a declared `+` somebody
        // wrote for a struct
        //
        // an undeterminable operand says nothing either way, so it is admitted for the reason the
        // binary rule admits one
        if (op->type == Token::Type::t_op_sub || op->type == Token::Type::t_op_add) {
            return operand.type.is_integer_type()
                || operand.type.is_floating_type()
                || is_undetermined_type(operand.type)
                || operand.type.is_type_param();
        }

        // **`!` is two questions with one answer.** over a `bool` it is the negation gen_unary_expr
        // emits; over anything that may be absent it is the *presence test* `== null` already is, and
        // it asks destination_admits_null - the identical predicate binary_has_builtin_meaning's
        // presence arm reads - rather than enumerating nullable, ptr, class and weak a second time.
        // that is the whole of what keeps `!$maybe` and `$maybe == null` from drifting apart
        if (op->type == Token::Type::t_exclamation) {
            return operand.type.is_boolean_type()
                || destination_admits_null(operand.type)
                || is_undetermined_type(operand.type)
                || operand.type.is_type_param();
        }

        // bitwise not, integer-only, the unary half of the bitwise five. a float, a bool and a
        // pointer fall through to TypeChecker's "not supported" the way `& | ^ << >>` do. a wrapped
        // optional is an integer type, so it has to be excluded here the way the binary arm excludes
        // one - CreateNot of an `{ i1, i32 }` pair is not the not of the payload
        if (op->type == Token::Type::t_tilde) {
            return (operand.type.is_integer_type() && !operand.type.is_wrapped_optional())
                || is_undetermined_type(operand.type)
                || operand.type.is_type_param();
        }

        return false;
    }

    std::optional<std::string> binary_operand_refusal(
        const Operator *op, const OperandFacts &lhs_facts, const OperandFacts &rhs_facts)
    {
        if (op == nullptr) {
            return std::nullopt;
        }

        const ValueType &lhs = lhs_facts.type;
        const ValueType &rhs = rhs_facts.type;

        // comparing against null only means something on an address. `$p == null` would read the int32
        // at address zero - exactly the crash the check is meant to prevent - so it is rejected and
        // `$p:$ == null` is the way to ask
        // (`$p:$ == null` is the way to ask)
        if (lhs_facts.is_null != rhs_facts.is_null) {
            const ValueType &other = lhs_facts.is_null ? rhs : lhs;

            // a class handle is itself the address, so it is compared directly - there is no slot to
            // peel to and `:$` on it would ask about the variable rather than the object.
            //
            // and **anything nullable**, which is what generalising the flag added here: a `T?` is
            // exactly the type that may be absent, whatever `T` is, so asking is always meaningful. for
            // a wrapped one - `int32?`, a nullable struct - it is a tag test rather than an address
            // comparison, and TypeLowering::gen_has_value is the one place that tells the two apart
            //
            // a weak too: it answers whether the reference was ever taken. **not** whether its object is
            // still alive, which is `strong($w)` - so the refusal is worth keeping distinct from the
            // ones above rather than folded into a single "cannot compare"
            //
            // asked of the two owners rather than enumerating them: a type that may be *absent* is
            // AST::destination_admits_null's answer, and one that has a spare null value to be absent
            // *as* is ValueType::has_null_representation's. a kind added to either reaches this refusal
            // and binary_has_builtin_meaning's presence arm together, which is the whole point - one of
            // them claiming a lowering the other refuses is a comparison with two meanings
            //
            // a non-nullable class has a spare null value (the handle) and still cannot be absent,
            // so it joins the struct/enum sentence rather than slipping through on the representation
            if (!destination_admits_null(other)
                && (!other.has_null_representation() || other.is_class())) {
                // an enum is on the struct's side of this: both are values with a layout and no spare
                // null, so `T?` is the answer for either, where an address wants `:$`. a class is the
                // same question about absence, even though the handle could encode one
                return other.is_struct() || other.is_enum() || other.is_class()
                    ? fmt::format("cannot compare '{}' against null - it is always there, write "
                        "'{}?' if it may be absent",
                        other.get_type_desciption(), other.get_type_desciption())
                    : fmt::format("cannot compare '{}' against null - null-check the address with ':$'",
                        other.get_type_desciption());
            }

            return std::nullopt;
        }

        // pointer arithmetic needs a stride. two pointers subtract, or one is offset by a count -
        // either way the pointee's size has to be known. `$p:$ + 1` on an incomplete type is the
        // case; indexing `$p:$[i]` asks the same question through AST::incomplete_stride_refusal
        if (op->type == Token::Type::t_op_add || op->type == Token::Type::t_op_sub) {
            if (lhs.is_pointer()) {
                if (auto refusal = incomplete_stride_refusal(lhs)) {
                    return refusal;
                }
            }

            if (rhs.is_pointer()) {
                if (auto refusal = incomplete_stride_refusal(rhs)) {
                    return refusal;
                }
            }
        }

        // comparing an address against a non-address. codegen lowers a pointer comparison to an icmp
        // over two pointers, and llvm asserts outright when the operand types differ - so without this
        // the compiler aborted with "Both operands to ICmp instruction are not of the same type!" and
        // no location at all
        //
        // scoped to comparisons: `$p:$ + 1` mixes a pointer and an int legitimately, because arithmetic
        // on an address is offsetting rather than comparing
        if (lhs.is_pointer() != rhs.is_pointer() && !lhs.is_void() && !rhs.is_void()
            && op->is_comparison()) {
            return fmt::format(
                "cannot compare '{}' against '{}' - an address only compares against another address",
                lhs.get_type_desciption(), rhs.get_type_desciption());
        }

        // a vector has no implicit broadcast. a literal beside one used to look like integer
        // arithmetic on the lane type and then fail in codegen; the sentence names splat.
        // `&&` / `||` are not this: they are not builtin, and the unsupported-operand
        // sentence is the one that says so. a second sentence here would be two errors
        if (lhs.is_simd() || rhs.is_simd()) {
            if (op->type == Token::Type::t_logical_and || op->type == Token::Type::t_logical_or) {
                return std::nullopt;
            }

            const auto applied = [&](const char *why) {
                return fmt::format(
                    "cannot apply '{}' to '{}' and '{}' - {}",
                    op->spelling, lhs.get_type_desciption(), rhs.get_type_desciption(), why);
            };

            if (op->is_shift()) {
                // legal: an integer vector and a scalar integer count. anything else is a
                // sentence, and the position failure is named ahead of the lane failure
                const bool position = lhs.is_simd() && !rhs.is_simd();
                const bool count_is_integer = rhs.is_integer_type() && !rhs.is_wrapped_optional();
                const bool lane_shifts = position
                    && simd_lane_op_lowers(op->type, ValueType::make_mutable(lhs.simd_element()));

                if (position && lane_shifts && count_is_integer) {
                    return std::nullopt;
                }

                if (!position) {
                    return applied("a vector shift takes a scalar count on the right");
                }

                return applied("a vector shift takes an integer vector and an integer count");
            }

            if (lhs.is_simd() != rhs.is_simd()) {
                return applied("there is no implicit broadcast; write simd::splat");
            }

            if (ValueType::make_mutable(lhs) != ValueType::make_mutable(rhs)) {
                return applied("both vectors have to be the same type");
            }

            const ValueType lane = ValueType::make_mutable(lhs.simd_element());

            if (simd_lane_op_lowers(op->type, lane)) {
                return std::nullopt;
            }

            // still a built-in so a declared `operator /` cannot claim the use site; the
            // sentence is this one rather than NoMatchingOverload
            const char *kind = lane.is_integer_type() ? "integer"
                : lane.is_floating_type() ? "float"
                : lane.is_boolean_type() ? "bool"
                : nullptr;

            if (kind == nullptr) {
                return fmt::format(
                    "operator '{}' is not supported on vectors", op->spelling);
            }

            return fmt::format(
                "operator '{}' is not supported on {} vectors", op->spelling, kind);
        }

        return std::nullopt;
    }

    std::string binary_unsupported_operands(
        const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs)
    {
        return fmt::format("operator '{}' is not supported on operands of type '{}' and '{}'",
            op != nullptr ? op->spelling : "?",
            lhs.type.get_type_desciption(),
            rhs.type.get_type_desciption());
    }

    std::optional<ValueType> common_numeric_type(const ValueType &lhs, const ValueType &rhs)
    {
        const bool lhs_numeric = lhs.is_integer_type() || lhs.is_floating_type();
        const bool rhs_numeric = rhs.is_integer_type() || rhs.is_floating_type();

        // both predicates already gate on is_primitive(), so get_primitive_type() below is answerable
        if (!lhs_numeric || !rhs_numeric || lhs.get_primitive_type() == rhs.get_primitive_type()) {
            return std::nullopt;
        }

        if (lhs.is_integer_type() && rhs.is_floating_type()) {
            return rhs;
        }

        if (lhs.is_floating_type() && rhs.is_integer_type()) {
            return lhs;
        }

        return get_primitive_size(lhs.get_primitive_type()) > get_primitive_size(rhs.get_primitive_type())
            ? lhs
            : rhs;
    }

    bool binary_reconciles_operands(const Operator *op)
    {
        // a null operator is every symbol this language spells but two, so the safe answer is the
        // common one: a caller with nothing to ask reconciles, exactly as it did before there was
        // anything to ask
        return op == nullptr || !op->is_shift();
    }

    ValueType binary_operation_type(const Operator *op, const ValueType &lhs, const ValueType &rhs)
    {
        if (!binary_reconciles_operands(op)) {
            return lhs;
        }

        return common_numeric_type(lhs, rhs).value_or(lhs);
    }

    ValueType builtin_unary_result(Token::Type op, const ValueType &operand)
    {
        if (op == Token::Type::t_exclamation) {
            if (operand.is_simd()) {
                return operand;
            }

            return ValueType(ValueTypePrimitive::t_bool);
        }

        return operand;
    }

    ValueType builtin_binary_result(const Operator *op, const ValueType &lhs, const ValueType &rhs)
    {
        if (op != nullptr && op->is_comparison()) {
            if (lhs.is_simd() || rhs.is_simd()) {
                const ValueType &vec = lhs.is_simd() ? lhs : rhs;

                return ValueType::make_simd(
                    ValueType(ValueTypePrimitive::t_bool), vec.simd_length());
            }

            return ValueType(ValueTypePrimitive::t_bool);
        }

        return binary_operation_type(op, lhs, rhs);
    }

    std::optional<std::string> shift_count_refusal(const ValueType &shifted, uint64_t count)
    {
        if (shifted.is_simd()) {
            return shift_count_refusal(shifted.simd_element(), count);
        }

        if (!shifted.is_integer_type()) {
            return std::nullopt;
        }

        const unsigned bits = get_integer_size(shifted.get_primitive_type()).bit_width();

        if (count < bits) {
            return std::nullopt;
        }

        return fmt::format(
            "this shifts a '{}' by {} or more bits, which at runtime is undefined; here it is simply "
            "refused.", shifted.get_type_desciption(), bits);
    }
};
