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

    // leftover `#[wasm: import]`, file-scope `#[wasm: export "memory"]` (writes
    // `bundle.wasm_export_memory`), `#[wasm: import]` on every declaration, and seating
    // `FunctionDeclNode::import_module`. TypeChecker asks once, like check_exports
    void check_wasm_surface(Collector &collector, Bundle &bundle, bool targeting_wasm);
};

#endif
