#include "AST/ASTAbi.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTCollector.h"
#include "AST/ASTExternInterface.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTValueType.h"
#include "AST/AttributeNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "Compiler/Fnv.h"

#include <fmt/core.h>
#include <string>
#include <unordered_set>

namespace
{
    // length-prefixed so `P` of 12 properties and `P1` of 2 are not one fingerprint
    uint64_t fold(uint64_t hash, const std::string &text)
    {
        return Compiler::fnv1a64(fmt::format("{}:{}", text.size(), text), hash);
    }

    // every level, qualifiers included. what a signature drops is AST::c_abi_erased alone
    void fold_type(
        uint64_t &hash,
        const AST::ValueType &type,
        std::unordered_set<std::string> &seen
    )
    {
        hash = fold(hash, fmt::format("{}{}", type.is_const() ? "c" : "-", type.is_nullable() ? "?" : "-"));

        const AST::ValueType bare = AST::ValueType::make_mutable(
            AST::ValueType::make_non_nullable(type));

        if (bare.is_pointer()) {
            hash = fold(hash, "ptr");
            fold_type(hash, bare.pointee(), seen);
            return;
        }

        if (bare.is_c_function()) {
            const std::string key = bare.get_mangled_name();
            if (!seen.insert(key).second) {
                hash = fold(hash, "fn-rec");
                return;
            }

            hash = fold(hash, "fn");
            const AST::CallableSignature erased = AST::c_abi_erased(bare.signature());
            fold_type(hash, erased.return_type, seen);

            for (const AST::ValueType &parameter : erased.parameter_types) {
                fold_type(hash, parameter, seen);
            }

            return;
        }

        if (bare.has_complex_type()) {
            const AST::ComplexType *ct = bare.get_complex_type();
            hash = fold(hash, ct->namespaced_name());

            if (ct->is_opaque_kind() || ct->is_interface_kind() || ct->is_extern_interface_kind()) {
                return;
            }

            const std::string layout_key = "L:" + ct->mangled_token();
            if (!seen.insert(layout_key).second) {
                return;
            }

            hash = fold(hash, std::to_string(ct->property_count()));

            for (size_t i = 0; i < ct->property_count(); ++i) {
                const AST::ComplexType::Property &prop = ct->get_property(i);
                hash = fold(hash, prop.name);
                fold_type(hash, prop.type, seen);
            }

            return;
        }

        hash = fold(hash, bare.get_mangled_name());
    }
}

std::string AST::abi_table_symbol(const ComplexType &ct)
{
    return "__eco_abi_" + ct.mangled_token();
}

uint64_t AST::abi_fingerprint(const ComplexType &ct)
{
    uint64_t hash = Compiler::k_fnv_offset_basis;
    std::unordered_set<std::string> seen;

    for (FunctionDeclNode *requirement : extern_interface_requirements(ct)) {
        hash = fold(hash, requirement->func_name());
        hash = fold(hash, requirement->requirement_link_name());
        fold_type(hash, requirement->c_function_type(), seen);
    }

    return hash;
}

bool AST::type_exports_abi_table(const TypeDeclNode &node, bool exports_abi_tables)
{
    return node.attributes.get_first("export") != nullptr
        && !abi_table_export_refusal(node, exports_abi_tables).has_value();
}

std::optional<std::string> AST::abi_table_export_refusal(
    const TypeDeclNode &node, bool exports_abi_tables)
{
    const AttributeNode *export_attr = node.attributes.get_first("export");

    if (export_attr == nullptr) {
        return std::nullopt;
    }

    if (node.complex_type().extern_conformances().empty()) {
        return fmt::format(
            "'#[export]' on '{}' emits an ABI table, so the type has to answer an extern interface",
            node.type_name());
    }

    if (export_attr->value.has_value()) {
        return std::string(
            "an 'export' on a type cannot take a name - the table is named from the interface");
    }

    if (!exports_abi_tables) {
        return std::string("cannot export an extern-interface table from a wasm module");
    }

    return std::nullopt;
}

void AST::check_abi_exports(Collector &collector, Bundle &bundle, bool exports_abi_tables)
{
    for (auto &module_ptr : bundle.modules) {
        Module &module = *module_ptr;

        for (TypeDeclNode *node : module.nodes.of_type<TypeDeclNode>()) {
            if (node == nullptr) {
                continue;
            }

            const AttributeNode *export_attr = node->attributes.get_first("export");

            if (export_attr == nullptr) {
                continue;
            }

            if (auto refusal = abi_table_export_refusal(*node, exports_abi_tables)) {
                collector.collect_issue<Issue::GenericError>(
                    CodeRef { &module, export_attr->attribute_id.make_slice() },
                    *refusal);
            }
        }
    }
}
