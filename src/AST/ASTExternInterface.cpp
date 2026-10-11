#include "AST/ASTExternInterface.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTIssue.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTValueType.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>
#include <vector>

std::string AST::extern_slot_property_name(size_t ordinal, const std::string &requirement_name)
{
    return fmt::format("__s{}_{}", ordinal, requirement_name);
}

std::vector<AST::FunctionDeclNode *> AST::extern_interface_requirements(const ComplexType &ct)
{
    std::vector<FunctionDeclNode *> out;

    // allow-list: a requirement is a written method. anything else in `_methods` is not a slot
    for (FunctionDeclNode *requirement : ct.methods()) {
        if (requirement != nullptr && requirement->member_kind == MemberKind::t_method) {
            out.push_back(requirement);
        }
    }

    return out;
}

std::optional<std::string> AST::extern_interface_uninitialized_refusal(const VarDeclNode &decl)
{
    if (decl.init_expr != nullptr || decl.binds_unwrapped) {
        return std::nullopt;
    }

    if (!decl.has_type()) {
        return std::nullopt;
    }

    return extern_interface_zero_refusal(decl.type_node()->type);
}

std::optional<std::string> AST::extern_interface_zero_refusal(const ValueType &type)
{
    if (type.is_nullable() || type.is_type_param() || !type.has_complex_type()) {
        return std::nullopt;
    }

    const ComplexType *ct = type.get_complex_type();

    if (ct == nullptr || !ct->is_extern_interface_kind()) {
        return std::nullopt;
    }

    return fmt::format(
        "an 'extern interface' value only comes from a binding; declare '{}?' or initialize it",
        type.get_type_desciption());
}

void AST::check_extern_interface_declarations(Collector &collector, Bundle &bundle)
{
    const bool has_erased = collector.core_types.has(CoreTypeKind::t_erased);

    for (auto &module_ptr : bundle.modules) {
        Module &module = *module_ptr;

        for (TypeDeclNode *node : module.nodes.of_type<TypeDeclNode>()) {
            if (node == nullptr || !node->complex_type().is_extern_interface_kind()) {
                continue;
            }

            if (!has_erased) {
                collector.collect_issue<Issue::GenericError>(
                    CodeRef { &module, node->name_token->make_slice() },
                    fmt::format(
                        "'extern interface {}' needs the standard library - its layout holds "
                        "'erased', which is unbound without it.",
                        node->type_name()));
            }

            for (FunctionDeclNode *requirement : extern_interface_requirements(node->complex_type())) {
                if (auto refusal = c_function_signature_refusal(
                        requirement->c_function_type().signature(), collector.core_types)) {
                    collector.collect_issue<Issue::GenericError>(
                        CodeRef { &module, requirement->declaration_site_token().make_slice() },
                        *refusal);
                }

                if (requirement->is_generic()) {
                    collector.collect_issue<Issue::GenericError>(
                        CodeRef { &module, requirement->declaration_site_token().make_slice() },
                        fmt::format(
                            "a requirement of '{}' cannot have type parameters.",
                            node->type_name()));
                }
            }

            for (FunctionDeclNode *member : node->complex_type().static_methods()) {
                if (member == nullptr || member->is_implicitly_generated) {
                    continue;
                }

                collector.collect_issue<Issue::GenericError>(
                    CodeRef { &module, member->declaration_site_token().make_slice() },
                    fmt::format(
                        "a requirement of '{}' cannot be static - an extern interface is "
                        "answered by statics on the conformer, not on the contract.",
                        node->type_name()));
            }
        }
    }
}

AST::ExternConformanceAnswer AST::extern_conformance_answer(
    const ComplexType &conformer,
    const FunctionDeclNode &requirement
)
{
    ExternConformanceAnswer out;
    const std::string name = requirement.func_name();
    const CallableSignature wanted = requirement.c_function_type().signature();

    FunctionDeclNode *match = nullptr;
    bool name_exists = false;

    for (FunctionDeclNode *candidate : find_static_functions(&conformer, name)) {
        if (candidate == nullptr) {
            continue;
        }

        name_exists = true;

        if (!c_function_signatures_match(candidate->c_function_type().signature(), wanted)) {
            continue;
        }

        match = candidate;
        break;
    }

    if (match == nullptr) {
        if (!name_exists) {
            out.refusal = fmt::format(
                "'{}' does not answer '{}' - declare a static function with that C signature.",
                conformer.namespaced_name(),
                name);
        }
        else {
            out.refusal = fmt::format(
                "'{}::{}' cannot answer '{}' - the C signatures do not match.",
                conformer.namespaced_name(),
                name,
                name);
        }

        return out;
    }

    if (match->is_generic()) {
        out.refusal = fmt::format(
            "'{}::{}' cannot answer a requirement because it is generic.",
            conformer.namespaced_name(),
            name);
        return out;
    }

    out.decl = match;
    return out;
}

void AST::check_extern_conformances(Collector &collector, Bundle &bundle)
{
    for (auto &module_ptr : bundle.modules) {
        Module &module = *module_ptr;

        for (TypeDeclNode *node : module.nodes.of_type<TypeDeclNode>()) {
            if (node == nullptr) {
                continue;
            }

            const ComplexType &conformer = node->complex_type();

            // the parser admits only an extern interface to this list
            for (const ValueType &iface_type : conformer.extern_conformances()) {
                const ComplexType *iface = iface_type.get_complex_type();

                if (iface == nullptr) {
                    continue;
                }

                for (FunctionDeclNode *requirement : extern_interface_requirements(*iface)) {
                    ExternConformanceAnswer answer = extern_conformance_answer(conformer, *requirement);

                    if (answer.refusal.has_value()) {
                        const TokenReference at = node->name_token.has_value()
                            ? node->name_token.value()
                            : requirement->declaration_site_token();
                        collector.collect_issue<Issue::GenericError>(
                            CodeRef { &module, at.make_slice() },
                            *answer.refusal);
                    }
                }
            }
        }
    }
}
