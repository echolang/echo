#include "AST/ASTImport.h"

#include "AST/ASTCollector.h"
#include "AST/ASTContext.h"
#include "AST/ASTFile.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"
#include "AST/ASTVisibility.h"
#include "AST/ConstDeclNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

#include <fmt/core.h>

namespace AST
{

std::string join_namespace_path(const std::vector<std::string> &parts)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) {
            out += ECO_NAMESPACE_SEPARATOR;
        }
        out += parts[i];
    }
    return out;
}

const File &file_of(const Context &context)
{
    return *context.file.file;
}

static CodeRef binding_ref(const File &file, const ImportBinding &binding)
{
    if (binding.span.has_value()) {
        return CodeRef { file.module, binding.span.value() };
    }

    if (binding.local_token.has_value()) {
        return CodeRef { file.module, binding.local_token->make_slice() };
    }

    return CodeRef { file.module, TokenSlice { file.module->tokens, 0, 0 } };
}

static void refuse_invisible(
    Collector &collector,
    const File &file,
    const ImportBinding &binding,
    Visibility visibility,
    const DeclarationOrigin &origin,
    const std::string &what,
    const std::optional<TokenReference> &declaration_token
)
{
    const DeclarationOrigin from { file.module, &file };
    if (visible_from(visibility, origin, from)) {
        return;
    }

    collector.collect_issue<Issue::InaccessibleDeclaration>(
        binding_ref(file, binding),
        visibility_refusal(visibility, origin, from, what),
        origin,
        declaration_token);
}

static bool try_resolve_binding(const File &file, ImportBinding &binding, Collector &collector, bool items_ready)
{
    if (binding.kind != ImportKind::t_unresolved || binding.reported) {
        return binding.kind != ImportKind::t_unresolved;
    }

    if (binding.path.empty()) {
        binding.reported = true;
        collector.collect_issue<Issue::UnknownUse>(
            binding_ref(file, binding),
            "A 'use' needs a path after it.");
        return false;
    }

    std::vector<std::string> parent_path = binding.path;
    const std::string last = parent_path.back();
    parent_path.pop_back();

    Namespace *parent = parent_path.empty()
        ? &collector.namespaces.root()
        : collector.namespaces.get(parent_path);

    if (parent == nullptr) {
        if (!items_ready) {
            return false;
        }

        binding.reported = true;
        collector.collect_issue<Issue::UnknownUse>(
            binding_ref(file, binding),
            fmt::format("Unknown namespace '{}'.", join_namespace_path(parent_path)));
        return false;
    }

    Namespace *as_namespace = collector.namespaces.get(binding.path);
    Symbol *symbol = collector.namespaces.find_symbol(last, *parent);
    const bool as_type = symbol != nullptr && symbol->type() == SymbolType::t_type;
    const bool as_constant = symbol != nullptr && symbol->type() == SymbolType::t_constant;
    const bool as_function = collector.functions.declares(last, *parent);
    const bool as_item = as_type || as_constant || as_function;

    if (as_namespace != nullptr && as_item) {
        binding.reported = true;
        collector.collect_issue<Issue::AmbiguousUse>(
            binding_ref(file, binding),
            fmt::format(
                "'{}' names both a namespace and a declaration. Qualify further, or pick one with 'as'.",
                join_namespace_path(binding.path)));
        return false;
    }

    if (as_namespace != nullptr) {
        binding.kind = ImportKind::t_namespace;
        binding.target_namespace = as_namespace;
        binding.target_name = last;
        return true;
    }

    if (as_item) {
        binding.kind = ImportKind::t_item;
        binding.target_namespace = parent;
        binding.target_name = last;

        const DeclarationOrigin from { file.module, &file };

        if (as_type) {
            auto *decl = symbol->node.unsafe_ptr<TypeDeclNode>();
            const ComplexType &layout = decl->complex_type();
            refuse_invisible(
                collector, file, binding, layout.visibility, layout.declared_in,
                decl->type_name(), decl->name_token);
        }

        if (as_constant) {
            auto *decl = symbol->node.unsafe_ptr<ConstDeclNode>();
            refuse_invisible(
                collector, file, binding, decl->visibility, decl->declared_in,
                decl->name(), decl->token_name);
        }

        if (as_function) {
            bool any_visible = false;
            const FunctionDeclNode *hidden = nullptr;
            for (FunctionDeclNode *decl : collector.functions.declared_overloads(last, *parent)) {
                if (visible_from(decl->visibility, decl->declared_in, from)) {
                    any_visible = true;
                    break;
                }
                if (hidden == nullptr) {
                    hidden = decl;
                }
            }

            if (!any_visible && hidden != nullptr) {
                refuse_invisible(
                    collector, file, binding, hidden->visibility, hidden->declared_in,
                    hidden->func_name(), hidden->name_token);
            }
        }

        return true;
    }

    if (!items_ready) {
        return false;
    }

    binding.reported = true;
    collector.collect_issue<Issue::UnknownUse>(
        binding_ref(file, binding),
        fmt::format("Nothing named '{}' exists.", join_namespace_path(binding.path)));
    return false;
}

const ImportBinding *file_import_for(const File &file, Collector &collector, std::string_view name)
{
    for (ImportBinding &binding : file.imports) {
        if (binding.local_name != name) {
            continue;
        }

        if (binding.kind == ImportKind::t_unresolved && !binding.reported) {
            try_resolve_binding(file, binding, collector, /*items_ready=*/false);
        }

        if (binding.kind != ImportKind::t_unresolved) {
            return &binding;
        }

        return nullptr;
    }

    return nullptr;
}

const ImportBinding *item_import_for(const File &file, Collector &collector, std::string_view name)
{
    const ImportBinding *imp = file_import_for(file, collector, name);

    if (imp == nullptr || imp->kind != ImportKind::t_item || imp->target_namespace == nullptr) {
        return nullptr;
    }

    return imp;
}

Namespace *imported_namespace_start(
    const File &file,
    Collector &collector,
    std::string_view name
)
{
    const ImportBinding *imp = file_import_for(file, collector, name);

    if (imp == nullptr || imp->target_namespace == nullptr) {
        return nullptr;
    }

    if (imp->kind != ImportKind::t_namespace && imp->kind != ImportKind::t_item) {
        return nullptr;
    }

    return imp->target_namespace;
}

Namespace *namespace_from_written_path(
    const File &file,
    Collector &collector,
    const std::vector<std::string> &parts,
    bool mint
)
{
    if (parts.empty()) {
        return nullptr;
    }

    Namespace *current = imported_namespace_start(file, collector, parts[0]);
    size_t index = 0;

    if (current != nullptr) {
        index = 1;
    }
    else if (mint) {
        return &collector.namespaces.retrieve(parts);
    }
    else {
        current = collector.namespaces.get(parts[0]);

        if (current == nullptr) {
            return nullptr;
        }

        index = 1;
    }

    for (; index < parts.size(); index++) {
        if (Namespace *child = collector.namespaces.get(*current, parts[index])) {
            current = child;
            continue;
        }

        if (!mint) {
            return nullptr;
        }

        current = &collector.namespaces.retrieve(*current, parts[index]);
    }

    return current;
}

bool apply_item_import(
    const File &file,
    Collector &collector,
    std::string_view written,
    const Namespace *&ns,
    std::string &imported_name)
{
    const ImportBinding *imp = item_import_for(file, collector, written);

    if (imp == nullptr) {
        return false;
    }

    ns = imp->target_namespace;

    if (imp->target_name != written) {
        imported_name = imp->target_name;
    }

    return true;
}

void finalize_file_imports(const File &file, Collector &collector)
{
    for (ImportBinding &binding : file.imports) {
        try_resolve_binding(file, binding, collector, /*items_ready=*/true);
    }
}

};
