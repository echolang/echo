#include <catch2/catch_test_macros.hpp>

#include <AST/ASTOperatorSemantics.h>
#include <AST/ASTOps.h>
#include <AST/ASTValueType.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

#include <algorithm>
#include <vector>

// AST::binary_has_builtin_meaning - "does ExprCodegen::gen_binary_expr lower this operator for these
// operands?"
//
// five consumers read it and only one of them turns a `false` into a message: the parser decides
// whether to look for a declared `operator`, AST::OperatorRewriter re-asks after substitution,
// Parser::parse_operatordecl refuses a declaration it answers yes for, AST::const_fold declines to fold
// what it answers no for, and AST::TypeChecker reports. So a `true` it cannot back up is not a wrong
// message, it is the compiler aborting with no location at all - which is what `1 << 2`, `$a == $b` over
// two bools and a weak handle asked against null each did.
//
// the table below is that promise written down. it is asked of the *facts* rather than of a parsed
// program, the way tests/ast_place_expr.cpp asks storage_of of the tag: OperandFacts is a plain
// aggregate, so a pair no small program produces can still be stated

using namespace AST;

namespace
{
    // the operand shapes the predicate distinguishes, each named for what it is rather than for the
    // type that stands in for it - a `weak<C>` and a class handle need a real ComplexType, so those two
    // come from a parse and the rest are built here
    struct Shapes
    {
        std::unique_ptr<Bundle> bundle;

        ValueType i32 = ValueType(ValueTypePrimitive::t_int32);
        ValueType u32 = ValueType(ValueTypePrimitive::t_uint32);
        ValueType f64 = ValueType(ValueTypePrimitive::t_float64);
        ValueType boolean = ValueType(ValueTypePrimitive::t_bool);
        ValueType unknown = ValueType::make_unknown();

        ValueType wrapped_i32 = ValueType::make_nullable(ValueType(ValueTypePrimitive::t_int32));
        ValueType address = ValueType::make_pointer(ValueType(ValueTypePrimitive::t_int32), true);

        ValueType structure;
        ValueType handle;
        ValueType weak_handle;
    };

    Shapes shapes()
    {
        Shapes out;

        out.bundle = EchoTests::tests_make_parsed_bundle(
            "struct P { int32 $v; }\n"
            "class C { int32 $v; }\n"
            "$p = P(1);\n"
            "C $c = C(2);\n");

        auto &module = out.bundle->modules.find_module("test");

        out.structure = EchoTests::type_named(module, "P")->value_type();
        out.handle = EchoTests::type_named(module, "C")->value_type();
        out.weak_handle = ValueType::make_weak(out.handle);

        return out;
    }

    OperandFacts value(const ValueType &type)
    {
        return OperandFacts { type, false };
    }

    // a written `null` carries no type of its own - what it means is decided by what it is beside,
    // which is exactly the asymmetry the two null arms of the predicate exist for
    OperandFacts written_null()
    {
        return OperandFacts { ValueType::make_unknown(), true };
    }

    const Operator *op_for(const OperatorRegistry &registry, Token::Type type)
    {
        return registry.get_operator(token_lit_symbol_string(type));
    }
}

TEST_CASE("binary_has_builtin_meaning promises exactly what codegen lowers", "[AST][operators]")
{
    OperatorRegistry registry;
    Shapes s = shapes();

    struct Row
    {
        Token::Type op;
        OperandFacts lhs;
        OperandFacts rhs;
        bool expected;
        const char *why;
    };

    const std::vector<Row> rows = {
        // **not decided yet.** an unresolved call's result type is unknown and a template body's
        // operands are a bare `T`. answering false here would have the parser build an operator call
        // out of a body whose type parameter nothing has bound
        { Token::Type::t_op_add, value(s.unknown), value(s.i32), true, "an undecided operand defers" },
        { Token::Type::t_op_shl, value(s.unknown), value(s.i32), true, "and defers whatever the symbol" },
        { Token::Type::t_op_add, value(s.i32), value(s.unknown), true, "either side" },

        // **presence.** one rule for all four shapes TypeLowering::gen_has_value answers for
        { Token::Type::t_logical_eq, value(s.wrapped_i32), written_null(), true, "a wrapped optional" },
        { Token::Type::t_logical_neq, value(s.address), written_null(), true, "an address" },
        { Token::Type::t_logical_eq, value(s.handle), written_null(), true, "a class handle" },
        { Token::Type::t_logical_eq, value(s.weak_handle), written_null(), true, "a weak handle" },
        { Token::Type::t_logical_eq, written_null(), value(s.weak_handle), true, "either way round" },
        { Token::Type::t_open_angle, value(s.handle), written_null(), false, "and only the two identity comparisons" },

        // **a written null against something that cannot be absent.** true for a primitive, so
        // AST::binary_operand_refusal is the one message; false for a named type, where a declared
        // `operator ==` is something an author could reach for
        { Token::Type::t_logical_eq, value(s.i32), written_null(), true, "a primitive keeps the built-in ==" },
        { Token::Type::t_logical_eq, value(s.structure), written_null(), false, "a struct looks for a declaration" },

        // classes
        { Token::Type::t_logical_eq, value(s.handle), value(s.handle), true, "two handles compare as addresses" },
        { Token::Type::t_open_angle, value(s.handle), value(s.handle), false, "and do not order" },
        { Token::Type::t_op_add, value(s.handle), value(s.handle), false, "and do not add" },
        { Token::Type::t_logical_eq, value(s.handle), value(s.i32), false, "a handle against a number is neither" },

        // weak - presence, and nothing else. `strong($w)` is how the object is asked after
        { Token::Type::t_logical_eq, value(s.weak_handle), value(s.weak_handle), false, "two weaks do not compare" },
        { Token::Type::t_logical_eq, value(s.weak_handle), value(s.handle), false, "nor a weak against a handle" },

        // addresses
        { Token::Type::t_logical_eq, value(s.address), value(s.address), true, "addresses compare" },
        { Token::Type::t_open_angle, value(s.address), value(s.address), true, "and order" },
        { Token::Type::t_op_sub, value(s.address), value(s.address), true, "and subtract to a distance" },
        { Token::Type::t_op_add, value(s.address), value(s.address), false, "two addresses cannot be added" },
        { Token::Type::t_op_add, value(s.address), value(s.i32), true, "an address offsets by an element count" },
        { Token::Type::t_op_mul, value(s.address), value(s.i32), false, "and is not scaled" },

        // **a value that may be absent has no arithmetic.** the one row here that pins a wrong *answer*
        // rather than a missing one: is_integer_type() is true for an `int32?`, so two of them used to
        // reach the integer arm and codegen compared the `{ i1, i32 }` pairs as numbers
        { Token::Type::t_logical_eq, value(s.wrapped_i32), value(s.wrapped_i32), false, "two optionals do not compare" },
        { Token::Type::t_op_add, value(s.wrapped_i32), value(s.i32), false, "nor add" },

        // integers
        { Token::Type::t_op_add, value(s.i32), value(s.i32), true, "integers add" },
        { Token::Type::t_op_pow, value(s.i32), value(s.i32), true, "and raise" },
        { Token::Type::t_xor, value(s.i32), value(s.u32), true, "and xor, the one bitwise operator that lowers" },
        { Token::Type::t_open_angle, value(s.i32), value(s.u32), true, "and order" },
        { Token::Type::t_op_shl, value(s.i32), value(s.i32), false, "`<<` parses and has no lowering" },
        { Token::Type::t_op_shr, value(s.i32), value(s.i32), false, "and `>>`" },
        { Token::Type::t_and, value(s.i32), value(s.i32), false, "and `&`" },
        { Token::Type::t_or, value(s.i32), value(s.i32), false, "and `|`" },
        { Token::Type::t_logical_and, value(s.i32), value(s.i32), false, "`&&` joins two bools, not two numbers" },

        // bools - a yes/no rather than a small number
        { Token::Type::t_logical_and, value(s.boolean), value(s.boolean), true, "bools join" },
        { Token::Type::t_logical_or, value(s.boolean), value(s.boolean), true, "either way" },
        { Token::Type::t_logical_eq, value(s.boolean), value(s.boolean), true, "and agree or not" },
        { Token::Type::t_logical_neq, value(s.boolean), value(s.boolean), true, "and disagree" },
        { Token::Type::t_open_angle, value(s.boolean), value(s.boolean), false, "and do not order" },
        { Token::Type::t_op_add, value(s.boolean), value(s.boolean), false, "and do not add" },
        { Token::Type::t_op_mod, value(s.boolean), value(s.boolean), false, "and do not divide" },
        { Token::Type::t_logical_eq, value(s.i32), value(s.boolean), false, "a number against a bool is neither pair" },

        // floats
        { Token::Type::t_op_add, value(s.f64), value(s.i32), true, "a float meeting a number promotes" },
        { Token::Type::t_op_div, value(s.f64), value(s.f64), true, "and divides" },
        { Token::Type::t_logical_leq, value(s.f64), value(s.f64), true, "and orders" },
        { Token::Type::t_op_pow, value(s.f64), value(s.f64), false, "`**` round-trips through llvm.pow for integers only" },
        { Token::Type::t_xor, value(s.f64), value(s.f64), false, "and there is no bitwise arm" },

        // named types have no built-in meaning at all, which is what sends a use site looking for a
        // declared `operator` - `string == string` is the standard library's, not the language's
        { Token::Type::t_logical_eq, value(s.structure), value(s.structure), false, "two structs look for a declaration" },
        { Token::Type::t_op_add, value(s.structure), value(s.structure), false, "whatever the symbol" },
    };

    for (const Row &row : rows) {
        const Operator *op = op_for(registry, row.op);

        INFO("operator " << token_lit_symbol_string(row.op) << " over '"
             << row.lhs.type.get_type_desciption() << "' and '"
             << row.rhs.type.get_type_desciption() << "' - " << row.why);

        REQUIRE(op != nullptr);
        REQUIRE(binary_has_builtin_meaning(op, row.lhs, row.rhs) == row.expected);
    }
}

TEST_CASE("a custom symbol never has a built-in meaning", "[AST][operators]")
{
    OperatorRegistry registry;
    Shapes s = shapes();

    // the first thing the predicate answers, and the reason a declared `avg` or `mm` is never weighed
    // against a lowering: the language spells no meaning for it, so there is nothing to fall through to
    Operator *custom = registry.find_or_declare({ "avg" });

    REQUIRE(custom != nullptr);
    REQUIRE_FALSE(binary_has_builtin_meaning(custom, value(s.i32), value(s.i32)));
    REQUIRE_FALSE(binary_has_builtin_meaning(nullptr, value(s.i32), value(s.i32)));
}

TEST_CASE("unary_has_builtin_meaning covers negation and both meanings of '!'", "[AST][operators]")
{
    OperatorRegistry registry;
    Shapes s = shapes();

    const Operator *neg = op_for(registry, Token::Type::t_op_sub);
    const Operator *bang = op_for(registry, Token::Type::t_exclamation);

    REQUIRE(neg != nullptr);
    REQUIRE(bang != nullptr);

    REQUIRE(unary_has_builtin_meaning(neg, value(s.i32)));
    REQUIRE(unary_has_builtin_meaning(neg, value(s.f64)));
    REQUIRE_FALSE(unary_has_builtin_meaning(neg, value(s.structure)));

    // `!` is two questions with one answer: negation over a bool, and the presence test over anything
    // that may be absent - the same test `== null` is, asked through the same predicate
    REQUIRE(unary_has_builtin_meaning(bang, value(s.boolean)));
    REQUIRE(unary_has_builtin_meaning(bang, value(s.wrapped_i32)));
    REQUIRE(unary_has_builtin_meaning(bang, value(s.address)));
    REQUIRE(unary_has_builtin_meaning(bang, value(s.weak_handle)));
    REQUIRE_FALSE(unary_has_builtin_meaning(bang, value(s.i32)));
    REQUIRE_FALSE(unary_has_builtin_meaning(bang, value(s.structure)));
}
