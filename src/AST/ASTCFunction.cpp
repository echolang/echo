#include "AST/ASTCFunction.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCompleteness.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTFile.h"
#include "AST/ASTFunctionEmission.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTSimd.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTVariadic.h"
#include "AST/AttributeNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

namespace
{
    bool name_is_listed(std::span<const char *const> names, std::string_view name)
    {
        for (const char *symbol : names) {
            if (name == symbol) {
                return true;
            }
        }

        return false;
    }

    std::optional<std::string> reserved_c_export_refusal(
        const std::string &name,
        std::span<const char *const> reserved_runtime,
        std::string_view reserved_runtime_prefix,
        std::span<const char *const> reserved_wasm_crt
    )
    {
        const bool runtime = name_is_listed(reserved_runtime, name)
            || (!reserved_runtime_prefix.empty()
                && name.rfind(reserved_runtime_prefix, 0) == 0);

        if (runtime) {
            return fmt::format(
                "cannot export as '{}' - that name is a compiler runtime symbol", name);
        }

        if (name_is_listed(reserved_wasm_crt, name)) {
            return fmt::format(
                "cannot export as '{}' - that name is a wasm C runtime symbol", name);
        }

        return std::nullopt;
    }

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

        if (type.is_primitive() || type.is_c_function()) {
            return std::nullopt;
        }

        if (type.is_pointer()) {
            const AST::ValueType pointee = AST::ValueType::make_mutable(
                AST::ValueType::make_non_nullable(type.pointee()));

            if (pointee.is_pointer()) {
                return component_refusal(pointee, is_return, core);
            }

            if (pointee.is_primitive() || pointee.is_c_function()) {
                return std::nullopt;
            }

            if (pointee.is_class() || pointee.is_weak() || pointee.is_interface()
                || AST::needs_destruction(pointee)) {
                return fmt::format(
                    "'{}' names Echo-owned storage a C caller cannot keep alive or free. "
                    "Pass a scalar or a pointer to bytes.",
                    type.get_type_desciption());
            }

            if (pointee.is_enum() && pointee.has_complex_type()
                && pointee.get_complex_type()->has_payload_case()) {
                return fmt::format(
                    "'{}' names an enum payload a C caller cannot copy. Pass a scalar.",
                    type.get_type_desciption());
            }

            return std::nullopt;
        }

        if (auto reason = AST::simd_crosses_c_refusal(type)) {
            return reason;
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

        if (AST::type_completeness(type) == AST::TypeCompleteness::t_incomplete) {
            return fmt::format(
                "'{}' is an incomplete type, so it cannot cross a C function-pointer boundary by "
                "value. Pass a 'ptr<{}>' instead.",
                type.get_type_desciption(),
                type.get_type_desciption());
        }

        if (type.is_struct() || type.is_enum()) {
            return fmt::format(
                "'{}' cannot cross a C function-pointer boundary by value - echoc and clang "
                "classify a struct differently, and the disagreement is silent. Pass a scalar "
                "or a pointer to bytes.",
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

std::optional<std::string> AST::export_refusal(
    const AST::FunctionDeclNode &decl,
    const AST::CoreTypes &core
)
{
    if (decl.is_static_method()) {
        return std::string(
            "cannot be exported because a static method belongs to a type C has no receiver for.");
    }

    if (decl.is_extern()) {
        return std::string(
            "an 'extern' function is imported, not exported");
    }

    if (decl.is_inline) {
        return std::string(
            "an exported function cannot be '#[inline]' - that would emit it into every unit "
            "rather than once under a raw symbol");
    }

    if (decl.is_implicitly_generated) {
        return std::string("a synthesized function cannot be exported");
    }

    if (auto reason = c_function_ref_refusal(decl)) {
        return fmt::format("cannot be exported because {}", *reason);
    }

    if (auto reason = c_function_signature_refusal(decl.c_function_type().signature(), core)) {
        return fmt::format("cannot be exported because {}", *reason);
    }

    return std::nullopt;
}

void AST::check_exports(
    Collector &collector,
    Bundle &bundle,
    std::span<const char *const> reserved_entry,
    std::span<const char *const> reserved_runtime,
    std::string_view reserved_runtime_prefix,
    std::span<const char *const> reserved_wasm_crt
)
{
    constexpr const char *k_export_on_function = "an 'export' belongs on a function";

    auto attr_ref = [](const Module &module, const AttributeNode &attr) {
        return CodeRef { &module, attr.attribute_id.make_slice() };
    };

    auto name_ref = [](const Module &module, const FunctionDeclNode &node) {
        return CodeRef { &module, node.name_token.value().make_slice() };
    };

    std::unordered_map<std::string, const FunctionDeclNode *> exported_names;
    std::unordered_set<std::string> extern_symbols;

    for (auto &module_ptr : bundle.modules) {
        Module &module = *module_ptr;
        for (FunctionDeclNode *node : module.nodes.of_type<FunctionDeclNode>()) {
            if (node != nullptr && node->extern_symbol.has_value()) {
                extern_symbols.insert(*node->extern_symbol);
            }
        }
    }

    for (auto &module_ptr : bundle.modules) {
        Module &module = *module_ptr;

        for (File &file : module.files()) {
            if (file.root == nullptr) {
                continue;
            }

            for (AttributeNode *attr : file.root->pending_attributes()) {
                if (attr != nullptr && attr->attribute_id.value() == "export") {
                    collector.collect_issue<Issue::GenericError>(
                        attr_ref(module, *attr),
                        k_export_on_function);
                }
            }
        }

        for (TypeDeclNode *node : module.nodes.of_type<TypeDeclNode>()) {
            if (node == nullptr) {
                continue;
            }

            if (auto *export_attr = node->attributes.get_first("export")) {
                collector.collect_issue<Issue::GenericError>(
                    attr_ref(module, *export_attr),
                    k_export_on_function);
            }
        }

        for (FunctionDeclNode *node : module.nodes.of_type<FunctionDeclNode>()) {
            if (node == nullptr) {
                continue;
            }

            if (!node->export_name.has_value() || !node->name_token.has_value()) {
                continue;
            }

            if (auto refusal = export_refusal(*node, collector.core_types)) {
                collector.collect_issue<Issue::GenericError>(
                    name_ref(module, *node),
                    std::move(refusal.value()));
                continue;
            }

            if (const auto reserved = reserved_c_export_refusal(
                    *node->export_name,
                    reserved_runtime,
                    reserved_runtime_prefix,
                    reserved_wasm_crt)) {
                collector.collect_issue<Issue::GenericError>(
                    name_ref(module, *node),
                    *reserved);
                continue;
            }

            if (extern_symbols.count(*node->export_name) > 0) {
                collector.collect_issue<Issue::GenericError>(
                    name_ref(module, *node),
                    fmt::format(
                        "cannot export as '{}' - an 'extern' in this program already binds that "
                        "C symbol",
                        *node->export_name));
                continue;
            }

            if (name_is_listed(reserved_entry, *node->export_name)) {
                collector.collect_issue<Issue::GenericError>(
                    name_ref(module, *node),
                    fmt::format(
                        "cannot export as '{}' - that name is the program's entry symbol",
                        *node->export_name));
                continue;
            }

            auto [it, inserted] = exported_names.emplace(*node->export_name, node);

            if (!inserted && it->second != node && it->second->name_token.has_value()) {
                collector.collect_issue<Issue::DuplicateExportName>(
                    name_ref(module, *node),
                    *node->export_name,
                    it->second->name_token.value());
            }
        }
    }
}

std::vector<AST::FunctionDeclNode *> AST::function_ref_candidates(
    const AST::FunctionRefExprNode &node,
    AST::Collector &collector
)
{
    if (node.is_static()) {
        auto statics = find_static_functions(
            collector,
            node.static_owner,
            node.lookup_name(),
            node.token_name
        );

        if (!statics.empty()) {
            return statics;
        }

        // a method of that name, so `&Type::method` is refused by c_function_ref_refusal
        // rather than as an unknown name. after the closed search, never as a receiver walk
        if (node.static_owner.has_complex_type()) {
            return find_member_functions(node.static_owner.get_complex_type(), node.lookup_name());
        }

        return {};
    }

    if (node.lookup_namespace == nullptr) {
        return {};
    }

    return collector.functions.overloads(node.lookup_name(), *node.lookup_namespace);
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
    AST::Collector &collector
)
{
    FunctionRefExprNode *ref = function_ref_of(expr);

    if (ref == nullptr) {
        return false;
    }

    const ValueType wanted = ValueType::make_mutable(ValueType::make_non_nullable(destination));

    const bool want_callable = wanted.is_callable();
    const bool want_c = wanted.is_c_function();

    if (!want_callable && !want_c) {
        return false;
    }

    // a C destination erases `const` on by-value parameters; an Echo callable does not.
    // arity alone would seat `&returns_int` at `function<void()>`
    auto callable_fits = [](const FunctionDeclNode &decl, const ValueType &dest) {
        return decl.callable_type() == dest;
    };

    // a unique candidate is already resolved by the parser, before the destination speaks.
    // a C destination leaves it as a C pointer; a callable destination is the same name
    // seated as `{ thunk, env }`, which is what `spawn(&answer)` is
    if (ref->resolved) {
        if (!want_callable || ref->decl == nullptr) {
            return false;
        }

        // flip even when the signature does not fit, so the type checker names two
        // Echo callables rather than a leftover C pointer against a function<...>
        ref->as_callable = true;
        return callable_fits(*ref->decl, wanted);
    }

    std::vector<FunctionDeclNode *> matches;

    for (FunctionDeclNode *candidate : function_ref_candidates(*ref, collector)) {
        const bool match = want_callable
            ? callable_fits(*candidate, wanted)
            : c_function_signatures_match(candidate->c_function_type().signature(), wanted.signature());

        if (match) {
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
    ref->as_callable = want_callable;
    return true;
}
