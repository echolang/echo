#include "AST/ASTArrayLiteral.h"

#include "AST/ASTCoreTypes.h"
#include "AST/ASTArgumentFit.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTTypeParam.h"
#include "AST/ASTVariadic.h"
#include "AST/ExprNode.h"

#include <fmt/core.h>

namespace
{
    AST::ArrayLiteralLookup refuse(std::string reason)
    {
        AST::ArrayLiteralLookup lookup;
        lookup.result = AST::ArrayLiteralLookup::Result::t_refused;
        lookup.refusal = std::move(reason);
        return lookup;
    }

    AST::ArrayLiteralLookup pending()
    {
        return AST::ArrayLiteralLookup {};
    }
}

AST::ArrayLiteralExprNode *AST::array_literal_of(AST::ExprNode *expr)
{
    if (expr == nullptr || expr->get_node_type() != NodeType::n_expr_array_literal) {
        return nullptr;
    }

    return static_cast<ArrayLiteralExprNode *>(expr);
}

bool AST::bind_array_literal_to(
    AST::ExprNode *expr,
    const AST::ValueType &destination,
    const AST::CoreTypes &core
)
{
    AST::ArrayLiteralExprNode *literal = array_literal_of(expr);

    if (literal == nullptr) {
        return false;
    }

    // **the variadic tail, ahead of every other reading of a bracket.** nothing is built here: the
    // elements are the call's own trailing arguments, so there is no `E` to unify them into and
    // nothing for AST::OperatorRewriter to expand. answering false is the point - the call is not
    // waiting on a literal, it already has everything it needs
    if (AST::is_variadic_args(destination, core)) {
        literal->is_variadic_pack = true;
        literal->expansion_decided = true;
        literal->bound_type = destination;
        return false;
    }

    const AST::ValueType wanted = AST::implicit_conversion_target(destination);

    if (is_undetermined_type(wanted)) {
        return false;
    }

    // idempotent, so the round that binds it and every round after read the same answer. the expansion
    // is decided against this type, and a second opinion arriving later would decide it twice
    if (!literal->bound_type.has_value()) {
        literal->bound_type = wanted;
    }

    return true;
}

AST::ArrayLiteralLookup AST::array_literal_type_for(
    const AST::ArrayLiteralExprNode &literal,
    const AST::CoreTypes &core,
    AST::TypeRegistry &types
)
{
    // **the unbound case comes first**, AST::iteration_plan_for's reason: `--no-stdlib` is a legitimate
    // program, and stdlib/core/array.eco is itself parsed by the compiler that would otherwise be
    // asserting the type it declares already exists
    AST::ComplexType *array_tmpl = core.declared_template(AST::CoreTypeKind::t_array);

    if (array_tmpl == nullptr) {
        // the one refusal here that cannot name the type the way the others do: there is no binding to
        // read a spelling off, which is exactly what it is reporting. the example is the stdlib's own
        return refuse(
            "an array literal with no declared type needs the core array type, and nothing in this "
            "program declares '#[core: array]'. it lives in the standard library, which this "
            "compilation left out - write the type, e.g. 'array<int32> $a = [...];'.");
    }

    // whatever `#[core: array]` names has to be applicable to one element type.
    // TypeRegistry::get_or_create_instantiation *asserts* both halves of this, so an oddly shaped
    // binding has to be a refusal here rather than an abort there
    if (!array_tmpl->is_generic() || array_tmpl->type_parameters.size() != 1) {
        return refuse(fmt::format(
            "'{}' is declared '#[core: array]' but takes no single element type, so an array "
            "literal cannot be built from it.",
            array_tmpl->name.value_or("the core array type")));
    }

    // an empty literal is the one shape the elements cannot answer for, which is the whole of what
    // book/concept/arrays.md says about it: it takes its type from where it is going, and here it is
    // going nowhere in particular
    if (literal.elements.empty()) {
        return refuse(fmt::format(
            "an empty array literal has nothing to go on - it takes its type from where it is going. "
            "Write the type, e.g. '{}<int32> $a = [];'.",
            core.spelling(AST::CoreTypeKind::t_array)));
    }

    if (literal.elements.front() == nullptr) {
        return pending();
    }

    // **the first element decides, and the rest are fitted to it.** not a common type over all of
    // them: the expansion writes one `$dest[] = element` per element, so AST::CallResolver and
    // AST::TypeChecker already judge every other element against this one - a second rule here would
    // be a second answer to a question they own, and would have to invent a widening the language has
    // not specified
    const AST::ValueType element = AST::value_result_type(*literal.elements.front());

    if (AST::is_undetermined_type(element)) {
        return pending();
    }

    // the element's `const` is a property of *the place it was read from*, not of what the array
    // holds - `const int32 $x = 1; $a = [$x];` fills an `array<int32>`, exactly as `$b = $x` declares
    // an `int32`. the declaration's own `const` is put back on top by the caller
    const std::vector<AST::ValueType> args { AST::ValueType::make_mutable(element) };

    if (const auto violation = AST::first_constraint_violation(array_tmpl->type_parameters, args)) {
        const auto *param = array_tmpl->type_parameters[*violation];

        return refuse(fmt::format(
            "'{}' cannot hold a '{}' - its element type is constrained to '{}'.",
            array_tmpl->name.value_or("the core array type"), args[*violation].get_type_desciption(),
            param->constraint_spelling));
    }

    AST::ArrayLiteralLookup lookup;
    lookup.result = AST::ArrayLiteralLookup::Result::t_ok;
    lookup.type = AST::ValueType::make_complex(types.get_or_create_instantiation(array_tmpl, args));

    return lookup;
}
