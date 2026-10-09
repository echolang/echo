#include "AST/ASTWasm.h"

#include "AST/ASTAttributeReader.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"

#include <fmt/core.h>

#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
AST::CodeRef attr_ref(const AST::Module &module, const AST::AttributeNode &attr)
{
    return AST::CodeRef { &module, attr.attribute_id.make_slice() };
}

AST::CodeRef name_ref(const AST::Module &module, const AST::FunctionDeclNode &node)
{
    return AST::CodeRef { &module, node.name_token.value().make_slice() };
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

void AST::check_wasm_surface(
    Collector &collector,
    Bundle &bundle,
    bool targeting_wasm,
    std::optional<const char *> entry_symbol
)
{
    std::unordered_map<std::string, const FunctionDeclNode *> exported_names;

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

                report_wasm_clause(collector, module, *attr, *clause, targeting_wasm);

                if (targeting_wasm && !clause->refusal.has_value()
                    && clause->kind == WasmClauseKind::t_import) {
                    collector.collect_issue<Issue::GenericError>(
                        attr_ref(module, *attr),
                        k_import_needs_extern);
                }
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

        for (FunctionDeclNode *node : module.nodes.of_type<FunctionDeclNode>()) {
            if (node == nullptr) {
                continue;
            }

            if (auto *wasm_attr = node->attributes.get_first("wasm")) {
                if (auto clause = wasm_clause_of(*wasm_attr)) {
                    report_wasm_clause(collector, module, *wasm_attr, *clause, targeting_wasm);

                    if (targeting_wasm && !clause->refusal.has_value()) {
                        if (clause->kind == WasmClauseKind::t_import && !node->is_extern()) {
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

            if (!node->export_name.has_value() || !node->name_token.has_value()) {
                continue;
            }

            if (auto refusal = export_refusal(*node, collector.core_types)) {
                collector.collect_issue<Issue::GenericError>(
                    name_ref(module, *node),
                    std::move(refusal.value()));
                continue;
            }

            if (entry_symbol.has_value() && *node->export_name == *entry_symbol) {
                collector.collect_issue<Issue::GenericError>(
                    name_ref(module, *node),
                    fmt::format(
                        "cannot export as '{}' - that name is the program's entry symbol",
                        *entry_symbol));
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
