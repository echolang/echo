#include "AST/ASTWasm.h"

#include "AST/ASTAttributeReader.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"

#include <fmt/core.h>

#include <utility>
#include <vector>

namespace
{
AST::CodeRef attr_ref(const AST::Module &module, const AST::AttributeNode &attr)
{
    return AST::CodeRef { &module, attr.attribute_id.make_slice() };
}

// off-row and a malformed payload. kind-specific shape errors stay with the caller
// that knows whether this landed on a function, at file root, or nowhere
void report_wasm_clause(
    AST::Collector &collector,
    const AST::Module &module,
    const AST::AttributeNode &attr,
    const AST::WasmClause &clause,
    bool targeting_wasm
)
{
    if (!targeting_wasm) {
        collector.collect_issue<AST::Issue::GenericError>(
            attr_ref(module, attr),
            "a '#[wasm:]' attribute is for '--target-os wasi'");
        return;
    }

    if (clause.refusal.has_value()) {
        collector.collect_issue<AST::Issue::GenericError>(
            attr_ref(module, attr),
            clause.refusal.value());
    }
}

constexpr const char *k_import_needs_extern =
    "only an 'extern' function is imported - a function with a body is exported with "
    "'#[export]'";

void report_leftover_import(
    AST::Collector &collector,
    const AST::Module &module,
    const AST::AttributeNode &attr,
    const AST::WasmClause &clause,
    bool targeting_wasm
)
{
    report_wasm_clause(collector, module, attr, clause, targeting_wasm);

    if (targeting_wasm && !clause.refusal.has_value()
        && clause.kind == AST::WasmClauseKind::t_import) {
        collector.collect_issue<AST::Issue::GenericError>(
            attr_ref(module, attr),
            k_import_needs_extern);
    }
}
};

std::optional<AST::WasmClause> AST::wasm_clause_of(const AttributeNode &attribute)
{
    if (attribute.attribute_id.value() != "wasm") {
        return std::nullopt;
    }

    WasmClause out;

    if (!attribute.value.has_value()) {
        out.refusal = "the 'wasm' attribute needs a value - write '#[wasm: import \"...\"]' or "
            "'#[wasm: export \"memory\"]'";
        return out;
    }

    const AttributeValue &written = attribute.value.value();
    AttributeReader reader("wasm");

    static const std::vector<std::pair<std::string, WasmClauseKind>> tags = {
        { "import", WasmClauseKind::t_import },
        { "export", WasmClauseKind::t_export },
    };

    if (!reader.tag(written, tags, "wasm clause", "import, export", out.kind)) {
        out.refusal = reader.refusals().front().message;
        return out;
    }

    std::optional<std::string> payload = reader.string(written);
    if (!payload.has_value()) {
        out.refusal = reader.refusals().empty()
            ? std::string("the 'wasm' attribute wants a string here")
            : reader.refusals().front().message;
        return out;
    }

    out.payload = std::move(*payload);

    if (out.kind == WasmClauseKind::t_export && out.payload != "memory") {
        out.refusal = fmt::format(
            "'{}' is not a wasm object export, expected one of: memory. a function is exported "
            "with '#[export]'",
            out.payload);
        return out;
    }

    return out;
}

void AST::check_wasm_surface(Collector &collector, Bundle &bundle, bool targeting_wasm)
{
    for (auto &module_ptr : bundle.modules) {
        Module &module = *module_ptr;

        for (File &file : module.files()) {
            if (file.root == nullptr) {
                continue;
            }

            for (AttributeNode *attr : file.root->pending_attributes()) {
                if (attr == nullptr) {
                    continue;
                }

                auto clause = wasm_clause_of(*attr);
                if (!clause.has_value() || clause->kind == WasmClauseKind::t_export) {
                    continue;
                }

                report_leftover_import(collector, module, *attr, *clause, targeting_wasm);
            }
        }

        // of_type, not file.root.children: a type body shares that ScopeNode, and the declaration pass
        // emplaces a second AttributeNode over the same tokens onto a scratch root. written_at_file_scope
        // is the parse-time fact that survives both
        for (AttributeNode *attr : module.nodes.of_type<AttributeNode>()) {
            if (attr == nullptr) {
                continue;
            }

            auto clause = wasm_clause_of(*attr);
            if (!clause.has_value() || clause->kind != WasmClauseKind::t_export) {
                continue;
            }

            report_wasm_clause(collector, module, *attr, *clause, targeting_wasm);

            if (!targeting_wasm || clause->refusal.has_value()) {
                continue;
            }

            if (attr->written_at_file_scope) {
                bundle.wasm_export_memory = true;
            } else {
                collector.collect_issue<Issue::GenericError>(
                    attr_ref(module, *attr),
                    "a wasm object export belongs at file scope");
            }
        }

        for (TypeDeclNode *node : module.nodes.of_type<TypeDeclNode>()) {
            if (node == nullptr) {
                continue;
            }

            auto *wasm_attr = node->attributes.get_first("wasm");
            if (wasm_attr == nullptr) {
                continue;
            }

            auto clause = wasm_clause_of(*wasm_attr);
            if (!clause.has_value()) {
                continue;
            }

            report_leftover_import(collector, module, *wasm_attr, *clause, targeting_wasm);
        }

        for (FunctionDeclNode *node : module.nodes.of_type<FunctionDeclNode>()) {
            if (node == nullptr) {
                continue;
            }

            auto *wasm_attr = node->attributes.get_first("wasm");
            if (wasm_attr == nullptr) {
                continue;
            }

            auto clause = wasm_clause_of(*wasm_attr);
            if (!clause.has_value()) {
                continue;
            }

            report_wasm_clause(collector, module, *wasm_attr, *clause, targeting_wasm);

            if (clause->refusal.has_value()) {
                continue;
            }

            if (clause->kind == WasmClauseKind::t_import && node->is_extern()) {
                node->import_module = clause->payload;
                continue;
            }

            if (!targeting_wasm) {
                continue;
            }

            if (clause->kind == WasmClauseKind::t_import) {
                collector.collect_issue<Issue::GenericError>(
                    attr_ref(module, *wasm_attr),
                    k_import_needs_extern);
            }
            else if (clause->kind == WasmClauseKind::t_export) {
                collector.collect_issue<Issue::GenericError>(
                    attr_ref(module, *wasm_attr),
                    "a wasm object export is not a function export");
            }
        }
    }
}
