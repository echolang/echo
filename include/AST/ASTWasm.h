#ifndef ASTWASM_H
#define ASTWASM_H

#pragma once

#include "AST/AttributeNode.h"

#include <optional>
#include <string>

namespace AST
{
    class Bundle;
    class Collector;
    class FunctionDeclNode;

    // **what a `#[wasm: ...]` value means.** nullopt is "not a wasm attribute".
    //
    // tag `import` carries a wasm module name (free text). tag `export` carries a closed
    // object list that today is `memory`. a function export is `#[export]`, not this.
    // `kind` is set whenever the tag parsed, including a refused payload, so a malformed
    // object export is still an object export
    enum class WasmClauseKind
    {
        t_import,
        t_export,
    };

    struct WasmClause
    {
        WasmClauseKind kind = WasmClauseKind::t_import;
        std::string payload;
        std::optional<std::string> refusal;
    };

    // the sole reading of a `wasm` attribute's tagged value. nullopt if the node is not
    // one. engaged with `refusal` when the tag, payload, or object name is wrong
    std::optional<WasmClause> wasm_clause_of(const AttributeNode &attribute);

    // `#[wasm: import "<module>"]`. the import name is extern_symbol. empty otherwise
    std::optional<std::string> wasm_import_module_of(const FunctionDeclNode &decl);

    // a file root writes `#[wasm: export "memory"]`
    bool wasm_exports_memory(Bundle &bundle);

    // leftover or misplaced `#[wasm:]`. reports, writes nothing.
    // `admits_wasm_surface` is TargetFacts::admits_wasm_surface()
    void check_wasm_surface(Collector &collector, Bundle &bundle, bool admits_wasm_surface);
};

#endif
