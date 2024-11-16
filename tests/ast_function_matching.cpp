#include <catch2/catch_test_macros.hpp>

#include <AST/ASTArgumentFit.h>
#include <AST/ASTFunctionMatcher.h>
#include <AST/ASTValueType.h>

using AST::ArgumentFit;
using AST::FunctionCandidate;
using AST::FunctionMatch;
using AST::ValueType;

namespace
{
    ValueType prim(AST::ValueTypePrimitive p) {
        return ValueType(p);
    }

    const ValueType t_int32 = prim(AST::ValueTypePrimitive::t_int32);
    const ValueType t_int64 = prim(AST::ValueTypePrimitive::t_int64);
    const ValueType t_uint8 = prim(AST::ValueTypePrimitive::t_uint8);
    const ValueType t_uint32 = prim(AST::ValueTypePrimitive::t_uint32);
    const ValueType t_int16 = prim(AST::ValueTypePrimitive::t_int16);
    const ValueType t_float32 = prim(AST::ValueTypePrimitive::t_float32);
    const ValueType t_float64 = prim(AST::ValueTypePrimitive::t_float64);
    const ValueType t_bool = prim(AST::ValueTypePrimitive::t_bool);

    // candidates carry no declaration: the matcher only ever compares parameter types, which is
    // the whole reason FunctionCandidate is a view rather than the declaration itself. that keeps
    // these tests free of a parse tree, a NodeCollection and a Collector
    FunctionCandidate candidate(std::vector<ValueType> params, bool is_generic = false) {
        return FunctionCandidate { nullptr, std::move(params), is_generic };
    }

    FunctionMatch match(const std::vector<FunctionCandidate> &candidates, const std::vector<ValueType> &args) {
        return AST::match_function(candidates, args, {});
    }
}

TEST_CASE( "argument_fit ranks conversions best to worst", "[fnmatch]" )
{
    REQUIRE( AST::argument_fit(t_int32, nullptr, t_int32) == ArgumentFit::t_exact );

    // a borrow widens to a nullable pointer over the same pointee
    const ValueType borrow = ValueType::make_pointer(t_int32, false);
    const ValueType nullable = ValueType::make_pointer(t_int32, true);
    REQUIRE( AST::argument_fit(borrow, nullptr, nullable) == ArgumentFit::t_widening );

    // a wider type of the same family keeps the value and stays a promotion
    REQUIRE( AST::argument_fit(t_int32, nullptr, t_int64) == ArgumentFit::t_promotion );
    REQUIRE( AST::argument_fit(t_float32, nullptr, t_float64) == ArgumentFit::t_promotion );
    REQUIRE( AST::argument_fit(t_uint8, nullptr, t_uint32) == ArgumentFit::t_promotion );

    // narrowing, a sign change and crossing families are all plain conversions
    REQUIRE( AST::argument_fit(t_int64, nullptr, t_int32) == ArgumentFit::t_conversion );
    REQUIRE( AST::argument_fit(t_int32, nullptr, t_uint32) == ArgumentFit::t_conversion );
    REQUIRE( AST::argument_fit(t_float64, nullptr, t_int32) == ArgumentFit::t_conversion );
    REQUIRE( AST::argument_fit(t_int32, nullptr, t_float64) == ArgumentFit::t_conversion );

    // an unknown on either side is "no information", never a mismatch
    REQUIRE( AST::argument_fit(t_int32, nullptr, ValueType::make_unknown()) == ArgumentFit::t_undetermined );
    REQUIRE( AST::argument_fit(ValueType::make_unknown(), nullptr, t_int32) == ArgumentFit::t_undetermined );

    // void as a source says nothing either; it is what a mixed-operand binary expression answers
    REQUIRE( AST::argument_fit(ValueType::void_type(), nullptr, t_int32) == ArgumentFit::t_undetermined );

    // a value cannot become a pointer without a place to take the address of, and no expression
    // was handed over here
    REQUIRE( AST::argument_fit(t_int32, nullptr, nullable) == ArgumentFit::t_none );
}

TEST_CASE( "arity filters before anything else", "[fnmatch]" )
{
    const std::vector<FunctionCandidate> candidates = {
        candidate({ t_int32 }),
        candidate({ t_int32, t_int32 }),
    };

    REQUIRE( match(candidates, { t_int32 }).outcome == FunctionMatch::Outcome::t_resolved );
    REQUIRE( match(candidates, { t_int32, t_int32 }).outcome == FunctionMatch::Outcome::t_resolved );

    // no candidate of this arity exists
    REQUIRE( match(candidates, { t_int32, t_int32, t_int32 }).outcome == FunctionMatch::Outcome::t_no_viable );
}

TEST_CASE( "a lone candidate is taken without consulting types", "[fnmatch]" )
{
    // this is what keeps a program with no overloads behaving exactly as it did before overload
    // resolution existed: a bad argument stays the type checker's diagnostic to report, with its
    // argument number and expected type, rather than becoming "no overload accepts these"
    const std::vector<FunctionCandidate> candidates = { candidate({ t_int32 }) };

    REQUIRE( match(candidates, { ValueType::make_pointer(t_float64, true) }).outcome == FunctionMatch::Outcome::t_resolved );
}

TEST_CASE( "an exact match beats a conversion", "[fnmatch]" )
{
    const std::vector<FunctionCandidate> candidates = {
        candidate({ t_int32 }),
        candidate({ t_float64 }),
    };

    REQUIRE( match(candidates, { t_float64 }).decl == candidates[1].decl );
    REQUIRE( match(candidates, { t_int32 }).decl == candidates[0].decl );
}

TEST_CASE( "no candidate accepts the arguments", "[fnmatch]" )
{
    const std::vector<FunctionCandidate> candidates = {
        candidate({ ValueType::make_pointer(t_int32, true) }),
        candidate({ ValueType::make_pointer(t_float64, true) }),
    };

    // an int32 value reaches neither pointer parameter, and with no expression there is no place
    // to borrow from
    auto result = match(candidates, { t_int32 });
    REQUIRE( result.outcome == FunctionMatch::Outcome::t_no_viable );
    REQUIRE( result.tied.size() == 2 );
}

TEST_CASE( "an equally good pair on different arguments is ambiguous", "[fnmatch]" )
{
    const std::vector<FunctionCandidate> candidates = {
        candidate({ t_int32, t_float64 }),
        candidate({ t_float64, t_int32 }),
    };

    // each is exact on one argument and a conversion on the other, so neither dominates
    auto result = match(candidates, { t_int32, t_int32 });
    REQUIRE( result.outcome == FunctionMatch::Outcome::t_ambiguous );
    REQUIRE( result.tied.size() == 2 );
}

TEST_CASE( "a concrete overload settles a tie against a template", "[fnmatch]" )
{
    const std::vector<FunctionCandidate> candidates = {
        candidate({ t_int32 }, true),
        candidate({ t_int32 }),
    };

    auto result = match(candidates, { t_int32 });
    REQUIRE( result.outcome == FunctionMatch::Outcome::t_resolved );
    REQUIRE( result.decl == candidates[1].decl );
}

TEST_CASE( "how well each argument fits is compared before generic-ness", "[fnmatch]" )
{
    // the template arrives already substituted with what it would be instantiated as, so it is
    // exact where the concrete overload would narrow. the specific-beats-generic rule must not
    // pre-empt that, or the call silently loses precision
    const std::vector<FunctionCandidate> candidates = {
        candidate({ t_float64 }, true),
        candidate({ t_int32 }),
    };

    auto result = match(candidates, { t_float64 });
    REQUIRE( result.outcome == FunctionMatch::Outcome::t_resolved );
    REQUIRE( result.decl == candidates[0].decl );
}

TEST_CASE( "an undetermined argument is neutral, not a tiebreaker", "[fnmatch]" )
{
    const std::vector<FunctionCandidate> candidates = {
        candidate({ t_int32, t_int32 }),
        candidate({ t_int32, t_float64 }),
    };

    // the second argument says nothing, so it can neither separate the candidates nor condemn
    // them. that is undecidable - a later pass may know more - and deliberately not "ambiguous",
    // which would be a claim that the program is wrong
    REQUIRE( match(candidates, { t_int32, ValueType::make_unknown() }).outcome == FunctionMatch::Outcome::t_undecidable );

    // an argument that *does* carry a type still separates them, undetermined neighbours or not
    const std::vector<FunctionCandidate> separable = {
        candidate({ t_int32, t_bool }),
        candidate({ t_float64, t_bool }),
    };
    auto decided = match(separable, { t_int32, t_bool });
    REQUIRE( decided.outcome == FunctionMatch::Outcome::t_resolved );
    REQUIRE( decided.decl == separable[0].decl );

    // and an int64 argument is a conversion to both, so those two tie on real information
    REQUIRE( match(separable, { t_int64, t_bool }).outcome == FunctionMatch::Outcome::t_ambiguous );
}

TEST_CASE( "a promotion beats a conversion, and an exact match beats both", "[fnmatch]" )
{
    const std::vector<FunctionCandidate> widen_or_cross = {
        candidate({ t_int64 }),
        candidate({ t_float64 }),
    };

    // int32 into int64 keeps both the value and the family; into float64 it changes families.
    // without the promotion tier these two ranked identically and the call was ambiguous
    REQUIRE( match(widen_or_cross, { t_int32 }).decl == widen_or_cross[0].decl );
    REQUIRE( match(widen_or_cross, { t_float32 }).decl == widen_or_cross[1].decl );

    const std::vector<FunctionCandidate> exact_or_promote = {
        candidate({ t_int64 }),
        candidate({ t_int32 }),
    };
    REQUIRE( match(exact_or_promote, { t_int32 }).decl == exact_or_promote[1].decl );

    // a narrowing and a sign change are both plain conversions, so they genuinely tie
    const std::vector<FunctionCandidate> equally_lossy = {
        candidate({ t_int16 }),
        candidate({ t_uint32 }),
    };
    REQUIRE( match(equally_lossy, { t_int32 }).outcome == FunctionMatch::Outcome::t_ambiguous );
}

TEST_CASE( "no candidates at all is distinct from none matching", "[fnmatch]" )
{
    // the caller turns this one into UnknownFunction and the other into NoMatchingOverload -
    // a name nobody declared is a different mistake from a name declared differently
    REQUIRE( match({}, { t_int32 }).outcome == FunctionMatch::Outcome::t_no_candidates );
}
