#include "AST/ASTCFunction.h"

#include "AST/ASTCoreTypes.h"
#include "AST/ASTFunctionEmission.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTVariadic.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/format.h>
#include <unordered_set>

namespace
{
    std::optional<std::string> component_refusal(
        const AST::ValueType &type,
        bool is_return,
        const AST::CoreTypes &core
    )
    {
        if (type.is_void()) {
            if (is_return) {
                return std::nullopt;
            }

            return std::string("a parameter cannot be 'void'.");
        }

        if (type.is_primitive() || type.is_pointer() || type.is_c_function()) {
            return std::nullopt;
        }

        if (AST::is_variadic_args(type, core)) {
            return std::string(
                "a C function pointer cannot end in a variadic tail - Echo has no spelling for "
                "C's '...'.");
        }

        // a type parameter is not a refusal - the signature is not concrete yet. the
        // instance's TypeChecker walk asks again with T bound, which is when a `Point`
        // or a class becomes a sentence. refusing here made `apply<T>(extern function<T(T)>)`
        // impossible on the template
        if (type.is_type_param() || AST::is_undetermined_type(type)) {
            return std::nullopt;
        }

        if (type.is_callable() || type.is_interface()) {
            return fmt::format(
                "'{}' has no C spelling - it is two words the other side has no declaration for.",
                type.get_type_desciption());
        }

        if (type.is_class() || type.is_weak()) {
            return fmt::format(
                "'{}' is a reference counted handle, and handing its address to C leaves nothing "
                "holding a reference. Pass a 'ptr<...>' instead.",
                type.get_type_desciption());
        }

        if (type.is_struct() || type.is_enum()) {
            return fmt::format(
                "'{}' cannot cross a C function-pointer boundary by value - echoc and clang "
                "classify a struct differently, and the disagreement is silent. Pass a "
                "'ptr<{}>' instead.",
                type.get_type_desciption(),
                type.get_type_desciption());
        }

        return fmt::format(
            "'{}' has no C spelling.",
            type.get_type_desciption());
    }

    // const on a by-value parameter is Echo's local write-protect, not C's. two signatures that
    // differ only by that bit are one C type, so bind_function_ref_to compares the erased form
    AST::CallableSignature abi_erased(const AST::CallableSignature &signature)
    {
        AST::CallableSignature erased;
        erased.return_type = AST::ValueType::make_mutable(
            AST::ValueType::make_non_nullable(signature.return_type));

        erased.parameter_types.reserve(signature.parameter_types.size());

        for (const auto &parameter : signature.parameter_types) {
            erased.parameter_types.push_back(
                AST::ValueType::make_mutable(AST::ValueType::make_non_nullable(parameter)));
        }

        return erased;
    }

    std::optional<std::string> type_refusal_walk(
        const AST::ValueType &type,
        const AST::CoreTypes &core,
        std::unordered_set<const AST::ComplexType *> &seen)
    {
        if (type.is_pointer()) {
            return type_refusal_walk(type.pointee(), core, seen);
        }

        if (type.is_weak()) {
            return type_refusal_walk(type.weak_target(), core, seen);
        }

        if (type.has_signature()) {
            if (type.is_c_function()) {
                if (auto reason = AST::c_function_signature_refusal(type.signature(), core)) {
                    return fmt::format(
                        "'{}' is not a C-callable signature - {}",
                        type.get_type_desciption(),
                        reason.value());
                }
            }

            if (auto nested = type_refusal_walk(type.signature().return_type, core, seen)) {
                return nested;
            }

            for (const auto &parameter : type.signature().parameter_types) {
                if (auto nested = type_refusal_walk(parameter, core, seen)) {
                    return nested;
                }
            }

            return std::nullopt;
        }

        if (!type.has_complex_type()) {
            return std::nullopt;
        }

        AST::ComplexType *ct = type.get_complex_type();

        if (ct == nullptr || !seen.insert(ct).second) {
            return std::nullopt;
        }

        if (ct->is_instantiated()) {
            for (const auto &arg : ct->instantiation_args) {
                if (auto nested = type_refusal_walk(arg, core, seen)) {
                    return nested;
                }
            }
        }

        for (size_t i = 0; i < ct->property_count(); i++) {
            if (auto nested = type_refusal_walk(ct->get_property_type(i), core, seen)) {
                return nested;
            }
        }

        for (AST::VarDeclNode *prop : ct->static_properties()) {
            if (prop != nullptr && prop->has_type()) {
                if (auto nested = type_refusal_walk(prop->type(), core, seen)) {
                    return nested;
                }
            }
        }

        return std::nullopt;
    }
};

std::optional<std::string> AST::c_function_type_refusal(
    const AST::ValueType &type,
    const AST::CoreTypes &core
)
{
    std::unordered_set<const ComplexType *> seen;

    return type_refusal_walk(type, core, seen);
}

bool AST::c_function_signatures_match(
    const AST::CallableSignature &a,
    const AST::CallableSignature &b
)
{
    return abi_erased(a) == abi_erased(b);
}

std::optional<std::string> AST::c_function_signature_refusal(
    const AST::CallableSignature &signature,
    const AST::CoreTypes &core
)
{
    if (auto reason = component_refusal(signature.return_type, true, core)) {
        return reason;
    }

    for (const auto &parameter : signature.parameter_types) {
        if (auto reason = component_refusal(parameter, false, core)) {
            return reason;
        }
    }

    return std::nullopt;
}

std::optional<std::string> AST::c_function_ref_refusal(const AST::FunctionDeclNode &decl)
{
    if (decl.implicit_arg_count() != 0) {
        if (decl.is_closure) {
            return std::string(
                "a closure has an environment C has nowhere to put - name the function and "
                "pass '&name'.");
        }

        return std::string(
            "a method or constructor has a receiver C has nowhere to put.");
    }

    if (decl.is_generic()) {
        return std::string(
            "a generic function has no single symbol to take the address of.");
    }

    const FunctionEmission emission = function_emission_kind(&decl);

    if (emission == FunctionEmission::t_no_symbol) {
        return std::string(
            "a builtin has no symbol at all - there is nothing to take the address of.");
    }

    if (emission == FunctionEmission::t_intrinsic) {
        return std::string(
            "an intrinsic is an LLVM name, not a function C can call.");
    }

    return std::nullopt;
}

std::vector<AST::FunctionDeclNode *> AST::function_ref_candidates(
    const AST::FunctionRefExprNode &node,
    AST::FunctionRegistry &functions
)
{
    if (node.is_static()) {
        auto statics = find_static_functions(node.static_owner.get_complex_type(), node.token_name.value());

        if (!statics.empty()) {
            return statics;
        }

        // a method of that name, so `&Type::method` is refused by c_function_ref_refusal
        // rather than as an unknown name
        return find_member_functions(node.static_owner.get_complex_type(), node.token_name.value());
    }

    if (node.lookup_namespace == nullptr) {
        return {};
    }

    return functions.overloads(node.token_name.value(), *node.lookup_namespace);
}

AST::FunctionRefExprNode *AST::function_ref_of(AST::ExprNode *expr)
{
    return const_cast<FunctionRefExprNode *>(function_ref_of(static_cast<const ExprNode *>(expr)));
}

const AST::FunctionRefExprNode *AST::function_ref_of(const AST::ExprNode *expr)
{
    const ExprNode *written = strip_implicit_casts(expr);

    if (written == nullptr || written->get_node_type() != NodeType::n_expr_function_ref) {
        return nullptr;
    }

    return static_cast<const FunctionRefExprNode *>(written);
}

bool AST::bind_function_ref_to(
    AST::ExprNode *expr,
    const AST::ValueType &destination,
    AST::FunctionRegistry &functions
)
{
    FunctionRefExprNode *ref = function_ref_of(expr);

    if (ref == nullptr || ref->resolved) {
        return false;
    }

    const ValueType wanted = ValueType::make_mutable(ValueType::make_non_nullable(destination));

    if (!wanted.is_c_function()) {
        return false;
    }

    std::vector<FunctionDeclNode *> matches;

    for (FunctionDeclNode *candidate : function_ref_candidates(*ref, functions)) {
        if (c_function_signatures_match(candidate->c_function_type().signature(), wanted.signature())) {
            matches.push_back(candidate);
        }
    }

    if (matches.size() != 1) {
        return false;
    }

    // bind even when the declaration cannot be addressed: TypeChecker reports the refusal
    // against a chosen name rather than an ambiguity
    ref->decl = matches[0];
    ref->resolved = true;
    return true;
}
