#include "AST/ASTConformance.h"

#include "AST/ASTOps.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/FunctionDeclNode.h"

#include <vector>

namespace
{
    AST::TypeSubstitution interface_parameter_substitution(const AST::ValueType &interface)
    {
        const AST::ComplexType *applied = interface.get_complex_type();
        const AST::ComplexType *tmpl = applied->template_or_self();

        if (!applied->is_instantiated() || tmpl->type_parameters.size() != applied->instantiation_args.size()) {
            return AST::TypeSubstitution {};
        }

        return AST::TypeSubstitution::positional(tmpl->type_parameters, applied->instantiation_args);
    }

    // intern-free substitution for operator requirements: a `const T&` is pointers and a
    // type parameter. first_constraint_violation substitutes generic applications before
    // it asks conforms_to, so this walk never has to intern
    AST::ValueType bind_without_intern(AST::ValueType declared, const AST::TypeSubstitution &subst)
    {
        if (declared.is_pointer()) {
            AST::ValueType inner = bind_without_intern(declared.pointee(), subst);
            AST::ValueType rebuilt = AST::ValueType::make_pointer(inner, declared.is_nullable());

            if (declared.is_const()) {
                rebuilt = AST::ValueType::make_const(rebuilt);
            }

            return rebuilt;
        }

        if (declared.is_type_param()) {
            const AST::ValueType *bound = subst.lookup(declared.get_type_param());

            if (bound == nullptr) {
                return declared;
            }

            AST::ValueType result = *bound;

            if (declared.is_const()) {
                result = AST::ValueType::make_const(result);
            }

            return result;
        }

        return declared;
    }

    bool requirement_answered_by_builtin(
        const AST::FunctionDeclNode *requirement,
        const AST::ValueType &asked,
        const AST::TypeSubstitution &subst)
    {
        const AST::Operator *op =
            AST::OperatorRegistry::predefined().get_operator(requirement->operator_spelling());

        if (op == nullptr) {
            return false;
        }

        const size_t arity = requirement->args.size();

        if (arity != 1 && arity != 2) {
            return false;
        }

        std::vector<AST::ValueType> peeled;
        peeled.reserve(arity);

        for (size_t i = 0; i < arity; i++) {
            const AST::ValueType bound =
                bind_without_intern(requirement->parameter_type(i), subst);

            // pending is `true` in binary_has_builtin_meaning so a template body can still parse.
            // structural conformance is a claim about a *settled* type, and that escape would
            // make every unanswered parameter look comparable
            if (AST::is_undetermined_type(bound)) {
                return false;
            }

            peeled.push_back(AST::ValueType::make_mutable(AST::value_type_of(bound)));
        }

        // the first operand is the type the interface is being asked of. without this, int32
        // would conform to comparable<float64> because float64 has `<`
        if (peeled[0] != AST::ValueType::make_mutable(asked)) {
            return false;
        }

        if (arity == 1) {
            AST::OperandFacts operand;
            operand.type = peeled[0];

            if (!AST::unary_has_builtin_meaning(op, operand)) {
                return false;
            }

            const AST::ValueType wanted =
                bind_without_intern(requirement->get_return_type(), subst);

            if (AST::is_undetermined_type(wanted)) {
                return false;
            }

            return AST::builtin_unary_result(op->type, peeled[0]) == wanted;
        }

        AST::OperandFacts lhs;
        lhs.type = peeled[0];
        AST::OperandFacts rhs;
        rhs.type = peeled[1];

        if (!AST::binary_has_builtin_meaning(op, lhs, rhs)) {
            return false;
        }

        const AST::ValueType wanted =
            bind_without_intern(requirement->get_return_type(), subst);

        if (AST::is_undetermined_type(wanted)) {
            return false;
        }

        return AST::builtin_binary_result(op, peeled[0], peeled[1]) == wanted;
    }

    bool structurally_conforms(const AST::ValueType &type, const AST::ValueType &interface)
    {
        // a type parameter is "not yet", and unknown is the same. neither is a settled type
        // that could answer an operator
        if (type.is_unknown() || type.is_type_param() || AST::contains_type_param(type)) {
            return false;
        }

        const AST::ComplexType *applied = interface.get_complex_type();

        if (applied == nullptr || !applied->is_interface_kind()) {
            return false;
        }

        if (!AST::interface_associated_types(applied).empty()) {
            return false;
        }

        const std::vector<AST::FunctionDeclNode *> &requirements = AST::interface_requirements(applied);

        // an empty interface is a marker, and a marker is something to opt into. there is
        // nothing structural about having no requirements
        if (requirements.empty()) {
            return false;
        }

        const AST::TypeSubstitution subst = interface_parameter_substitution(interface);

        for (const AST::FunctionDeclNode *requirement : requirements) {
            if (requirement == nullptr || !requirement->is_operator()) {
                return false;
            }

            if (!requirement_answered_by_builtin(requirement, type, subst)) {
                return false;
            }
        }

        return true;
    }
};

bool AST::conforms_to(const AST::ValueType &type, const AST::ValueType &interface)
{
    if (!interface.is_interface()) {
        return false;
    }

    if (type.has_complex_type()) {
        return AST::conforms_to(type.get_complex_type(), interface);
    }

    return structurally_conforms(type, interface);
}
