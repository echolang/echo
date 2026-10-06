#include "Compiler/Lsp/LspHints.h"

#include "AST/ASTBundle.h"
#include "AST/ASTNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "Compiler/Lsp/LspResolve.h"

#include <unordered_map>

namespace
{
    typedef Compiler::Lsp::PositionIndex::Entry Entry;
    typedef Compiler::Lsp::PositionIndex::EntryRole EntryRole;

    // longer than this and the hint pushes the code it annotates off the screen. the tooltip
    // still carries the whole type
    constexpr size_t k_max_hint_length = 40;

    AST::Span span_of_entry(const AST::File &file, const Entry &entry)
    {
        AST::Span span;
        span.file = &file;
        span.start = AST::Location{ entry.line, entry.column };
        span.end = AST::Location{ entry.line, entry.column + (entry.width > 0 ? entry.width : 1) };
        return span;
    }

    Compiler::Lsp::HighlightKind highlight_kind_of(const Entry &entry)
    {
        switch (entry.role) {
        case EntryRole::t_write:
            return Compiler::Lsp::HighlightKind::t_write;
        case EntryRole::t_parameter:
            return Compiler::Lsp::HighlightKind::t_write;
        case EntryRole::t_declaration:
            break;
        case EntryRole::t_use:
            return Compiler::Lsp::HighlightKind::t_read;
        }

        // a variable's declaration binds a value; a function's or a type's only names one
        const AST::NodeReference ref = AST::make_ref(entry.node);
        if (ref.has_type<AST::VarDeclNode>() && ref.get_ptr<AST::VarDeclNode>()->init_expr != nullptr) {
            return Compiler::Lsp::HighlightKind::t_write;
        }

        return Compiler::Lsp::HighlightKind::t_text;
    }

    // the parser gives a declaration that wrote no type a TypeNode with nothing written: no token,
    // no names. that is what an inlay hint is for
    bool has_inferred_type(const AST::VarDeclNode &decl)
    {
        const AST::TypeNode *type_node = decl.optional_type_node();
        if (type_node == nullptr || type_node->type_token.has_value() || !type_node->written_names.empty()) {
            return false;
        }

        return decl.has_type() && !decl.type().is_unknown();
    }
};

std::vector<Compiler::Lsp::Highlight> Compiler::Lsp::document_highlights(
    const Snapshot &snapshot,
    const AST::File &file,
    AST::Location location
)
{
    std::vector<Highlight> out;

    const PositionIndex::Entry *hit = snapshot.index.entry_at(&file, location.line, location.column);
    if (hit == nullptr || hit->node == nullptr) {
        return out;
    }

    std::unordered_map<AST::Node *, AST::Node *> cache;
    AST::Node *wanted = reference_target(hit->node, *snapshot.bundle, cache);
    if (wanted == nullptr) {
        return out;
    }

    for (const Entry &entry : snapshot.index.entries_of(&file)) {
        if (entry.node == nullptr || reference_target(entry.node, *snapshot.bundle, cache) != wanted) {
            continue;
        }

        // a declaration and a use can share a token (a constant's arena copy); one highlight each
        if (!out.empty() && out.back().range.start.line == entry.line
            && out.back().range.start.column == entry.column) {
            continue;
        }

        out.push_back(Highlight{ span_of_entry(file, entry), highlight_kind_of(entry) });
    }

    return out;
}

std::vector<Compiler::Lsp::InlayHint> Compiler::Lsp::inlay_hints(
    const Snapshot &snapshot,
    const AST::File &file,
    uint32_t first_line,
    uint32_t last_line
)
{
    std::vector<InlayHint> out;

    for (const Entry &entry : snapshot.index.entries_of(&file)) {
        if (entry.line < first_line || entry.line > last_line || entry.role != EntryRole::t_declaration) {
            continue;
        }

        const AST::NodeReference ref = AST::make_ref(entry.node);
        if (!ref.has_type<AST::VarDeclNode>()) {
            continue;
        }

        const AST::VarDeclNode &decl = *ref.get_ptr<AST::VarDeclNode>();
        if (decl.name_full().rfind("$__", 0) == 0 || !has_inferred_type(decl)) {
            continue;
        }

        const std::string type = decl.type().get_type_desciption();

        InlayHint hint;
        hint.position = AST::Location{ entry.line, entry.column };
        hint.label = type.size() > k_max_hint_length ? type.substr(0, k_max_hint_length - 3) + "..." : type;
        hint.tooltip = type;
        out.push_back(std::move(hint));
    }

    return out;
}
