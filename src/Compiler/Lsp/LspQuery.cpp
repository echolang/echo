#include "Compiler/Lsp/LspQuery.h"

#include "AST/ASTAccess.h"
#include "AST/ASTBundle.h"
#include "AST/ASTConstantExpander.h"
#include "AST/ASTFile.h"
#include "AST/ASTNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTSourceToken.h"
#include "AST/ASTValueType.h"
#include "AST/ConstDeclNode.h"
#include "AST/ConstRefExprNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NamespaceDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "Compiler/Lsp/LspUri.h"
#include "Compiler/SettledPath.h"
#include "Token.h"

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace
{
    AST::TypeDeclNode *type_decl_of(AST::Bundle &bundle, const AST::ValueType &type)
    {
        const AST::ValueType named = AST::target_type_of(type);
        if (!named.has_complex_type()) {
            return nullptr;
        }

        const AST::ComplexType *wanted = named.get_complex_type()->template_or_self();

        for (auto &module_ptr : bundle.modules) {
            for (AST::TypeDeclNode *decl : module_ptr->nodes.of_type<AST::TypeDeclNode>()) {
                if (&decl->complex_type() == wanted) {
                    return decl;
                }
            }
        }

        return nullptr;
    }

    AST::VarDeclNode *property_decl_of(AST::Bundle &bundle, const AST::MemberAccessNode &access)
    {
        const AST::ValueType base = access.base_target_type();
        if (!base.has_complex_type()) {
            return nullptr;
        }

        AST::TypeDeclNode *owner = type_decl_of(bundle, base);
        if (owner == nullptr) {
            return nullptr;
        }

        const std::string &name = access.get_member_name().value();
        for (AST::VarDeclNode *prop : owner->properties()) {
            if (prop->name() == name) {
                return prop;
            }
        }

        return nullptr;
    }

    AST::ConstDeclNode *constant_decl_of(AST::Bundle &bundle, AST::ConstRefExprNode &ref)
    {
        return AST::find_constant(
            bundle.collector.namespaces,
            ref.lookup_name(),
            ref.lookup_namespace,
            ref.is_qualified);
    }

    AST::Node *definition_target(AST::Node *node, AST::Bundle &bundle)
    {
        if (node == nullptr) {
            return nullptr;
        }

        const AST::NodeReference ref = AST::make_ref(node);

        if (ref.has_type<AST::VarNode>()) {
            return &ref.get_ptr<AST::VarNode>()->decl();
        }

        if (ref.has_type<AST::FunctionCallExprNode>()) {
            return ref.get_ptr<AST::FunctionCallExprNode>()->decl;
        }

        if (ref.has_type<AST::TypeNode>()) {
            return type_decl_of(bundle, ref.get_ptr<AST::TypeNode>()->type);
        }

        if (ref.has_type<AST::MemberAccessNode>()) {
            return property_decl_of(bundle, *ref.get_ptr<AST::MemberAccessNode>());
        }

        if (ref.has_type<AST::StaticPropertyExprNode>()) {
            return ref.get_ptr<AST::StaticPropertyExprNode>()->decl;
        }

        if (ref.has_type<AST::FunctionRefExprNode>()) {
            return ref.get_ptr<AST::FunctionRefExprNode>()->decl;
        }

        if (ref.has_type<AST::ConstRefExprNode>()) {
            return constant_decl_of(bundle, *ref.get_ptr<AST::ConstRefExprNode>());
        }

        return node;
    }

    AST::Node *canonical_target(AST::Node *node)
    {
        if (node == nullptr) {
            return nullptr;
        }

        const AST::NodeReference ref = AST::make_ref(node);
        if (ref.has_type<AST::FunctionDeclNode>()) {
            AST::FunctionDeclNode *fn = ref.get_ptr<AST::FunctionDeclNode>();
            if (fn->template_ref != nullptr) {
                return fn->template_ref;
            }
        }

        return node;
    }

    AST::Node *target_of(
        AST::Node *node,
        AST::Bundle &bundle,
        std::unordered_map<AST::Node *, AST::Node *> &cache
    )
    {
        auto found = cache.find(node);
        if (found != cache.end()) {
            return found->second;
        }

        AST::Node *target = canonical_target(definition_target(node, bundle));
        cache[node] = target;
        return target;
    }

    Compiler::Lsp::OutlineKind function_outline_kind(const AST::FunctionDeclNode &decl)
    {
        switch (decl.member_kind) {
        case AST::MemberKind::t_method:
        case AST::MemberKind::t_static_method:
        case AST::MemberKind::t_destructor:
        case AST::MemberKind::t_init:
            return Compiler::Lsp::OutlineKind::t_method;
        case AST::MemberKind::t_constructor:
            return Compiler::Lsp::OutlineKind::t_constructor;
        case AST::MemberKind::t_operator:
            return Compiler::Lsp::OutlineKind::t_operator;
        default:
            return decl.owner_type != nullptr
                ? Compiler::Lsp::OutlineKind::t_method
                : Compiler::Lsp::OutlineKind::t_function;
        }
    }

    Compiler::Lsp::OutlineKind type_outline_kind(const AST::TypeDeclNode &decl)
    {
        switch (decl.kind()) {
        case AST::ComplexTypeKind::t_class:
            return Compiler::Lsp::OutlineKind::t_class;
        case AST::ComplexTypeKind::t_interface:
            return Compiler::Lsp::OutlineKind::t_interface;
        case AST::ComplexTypeKind::t_enum:
            return Compiler::Lsp::OutlineKind::t_enum;
        case AST::ComplexTypeKind::t_struct:
        case AST::ComplexTypeKind::t_opaque:
            return Compiler::Lsp::OutlineKind::t_struct;
        }

        return Compiler::Lsp::OutlineKind::t_struct;
    }

    bool skip_generated_function(const AST::FunctionDeclNode &decl)
    {
        return decl.is_instantiated()
            || decl.is_implicitly_generated
            || decl.is_closure
            || decl.member_kind == AST::MemberKind::t_test;
    }

    bool skip_from_file_outline(const AST::FunctionDeclNode &decl)
    {
        return skip_generated_function(decl)
            || decl.is_member()
            || decl.is_constructor();
    }

    const TokenReference *name_token_of(AST::Node *node)
    {
        const AST::NodeReference ref = AST::make_ref(node);

        if (ref.has_type<AST::VarDeclNode>()) {
            return &ref.get_ptr<AST::VarDeclNode>()->token_varname;
        }

        if (ref.has_type<AST::FunctionDeclNode>()) {
            AST::FunctionDeclNode *fn = ref.get_ptr<AST::FunctionDeclNode>();
            if (fn->name_token.has_value()) {
                return &fn->name_token.value();
            }

            return nullptr;
        }

        if (ref.has_type<AST::TypeDeclNode>()) {
            AST::TypeDeclNode *type = ref.get_ptr<AST::TypeDeclNode>();
            if (type->name_token.has_value()) {
                return &type->name_token.value();
            }

            return nullptr;
        }

        if (ref.has_type<AST::ConstDeclNode>()) {
            return &ref.get_ptr<AST::ConstDeclNode>()->token_name;
        }

        if (ref.has_type<AST::ConstRefExprNode>()) {
            return &ref.get_ptr<AST::ConstRefExprNode>()->token_name;
        }

        return AST::source_token_of(*node);
    }

    bool entry_is_declaration(
        const Compiler::Lsp::PositionIndex::Entry &entry,
        AST::Node *wanted
    )
    {
        if (entry.node == wanted) {
            return true;
        }

        const TokenReference *decl_token = name_token_of(wanted);
        if (decl_token == nullptr || !decl_token->is_valid()) {
            return false;
        }

        return entry.line == decl_token->line()
            && entry.column == decl_token->char_offset();
    }

    std::optional<Compiler::Lsp::HoverAnswer> hover_of(AST::Node *node, AST::Bundle &bundle)
    {
        const AST::NodeReference ref = AST::make_ref(node);
        Compiler::Lsp::HoverAnswer answer;

        if (ref.has_type<AST::ConstRefExprNode>()) {
            AST::ConstRefExprNode *cref = ref.get_ptr<AST::ConstRefExprNode>();
            AST::ConstDeclNode *decl = AST::find_constant(
                bundle.collector.namespaces,
                cref->lookup_name(),
                cref->lookup_namespace,
                cref->is_qualified);
            if (decl != nullptr && decl->value != nullptr) {
                answer.type_description = "const " + decl->name()
                    + " : " + decl->value->result_type().get_type_desciption();
            }
            else {
                answer.type_description = "const " + cref->lookup_name();
            }

            answer.range = AST::span_of(cref->token_name);
            return answer;
        }

        if (ref.is_expression_node()) {
            AST::ExprNode *expr = static_cast<AST::ExprNode *>(node);
            AST::ExprNode *inner = AST::strip_implicit_casts(expr);
            answer.type_description = inner != nullptr
                ? inner->result_type().get_type_desciption()
                : expr->result_type().get_type_desciption();

            if (ref.has_type<AST::FunctionCallExprNode>()) {
                AST::FunctionCallExprNode *call = ref.get_ptr<AST::FunctionCallExprNode>();
                if (call->decl != nullptr) {
                    answer.signature = call->decl->signature_description()
                        + " -> " + call->decl->get_return_type().get_type_desciption();
                }
            }

            if (ref.has_type<AST::FunctionRefExprNode>()) {
                AST::FunctionRefExprNode *fnref = ref.get_ptr<AST::FunctionRefExprNode>();
                if (fnref->decl != nullptr) {
                    answer.signature = fnref->decl->signature_description()
                        + " -> " + fnref->decl->get_return_type().get_type_desciption();
                }
            }

            const TokenReference *token = AST::source_token_of(*node);
            if (token != nullptr && token->is_valid()) {
                answer.range = AST::span_of(*token);
            }

            return answer;
        }

        if (ref.has_type<AST::VarNode>()) {
            AST::VarDeclNode &decl = ref.get_ptr<AST::VarNode>()->decl();
            if (!decl.has_type()) {
                return std::nullopt;
            }

            answer.type_description = decl.type().get_type_desciption();
            answer.range = AST::span_of(ref.get_ptr<AST::VarNode>()->use_token());
            return answer;
        }

        if (ref.has_type<AST::VarDeclNode>()) {
            AST::VarDeclNode *decl = ref.get_ptr<AST::VarDeclNode>();
            if (!decl->has_type()) {
                return std::nullopt;
            }

            answer.type_description = decl->type().get_type_desciption();
            answer.range = AST::span_of(decl->token_varname);
            return answer;
        }

        if (ref.has_type<AST::FunctionDeclNode>()) {
            AST::FunctionDeclNode *fn = ref.get_ptr<AST::FunctionDeclNode>();
            answer.type_description = fn->signature_description()
                + " -> " + fn->get_return_type().get_type_desciption();
            if (fn->name_token.has_value()) {
                answer.range = AST::span_of(fn->name_token.value());
            }

            return answer;
        }

        if (ref.has_type<AST::TypeDeclNode>()) {
            AST::TypeDeclNode *type = ref.get_ptr<AST::TypeDeclNode>();
            answer.type_description = type->value_type().get_type_desciption();
            if (type->name_token.has_value()) {
                answer.range = AST::span_of(type->name_token.value());
            }

            return answer;
        }

        if (ref.has_type<AST::TypeNode>()) {
            AST::TypeNode *written = ref.get_ptr<AST::TypeNode>();
            answer.type_description = written->type.get_type_desciption();
            if (written->type_token.has_value()) {
                answer.range = AST::span_of(written->type_token.value());
            }

            return answer;
        }

        if (ref.has_type<AST::ConstDeclNode>()) {
            AST::ConstDeclNode *constant = ref.get_ptr<AST::ConstDeclNode>();
            if (constant->value != nullptr) {
                answer.type_description = constant->value->result_type().get_type_desciption();
            }
            else {
                answer.type_description = "const " + constant->name();
            }

            answer.range = AST::span_of(constant->token_name);
            return answer;
        }

        return std::nullopt;
    }

    AST::Span span_through_scope(AST::Span span, const AST::ScopeNode *scope)
    {
        if (scope == nullptr) {
            return span;
        }

        if (scope->token_brace.has_value()) {
            span = AST::union_span(span, AST::span_of(scope->token_brace.value()));
        }

        for (const AST::NodeReference &child : scope->children) {
            if (child.node() == nullptr) {
                continue;
            }

            const TokenReference *token = AST::source_token_of(*child.node());
            if (token != nullptr && token->is_valid()) {
                span = AST::union_span(span, AST::span_of(*token));
            }

            if (child.has_type<AST::ScopeNode>()) {
                span = span_through_scope(span, child.get_ptr<AST::ScopeNode>());
            }
        }

        return span;
    }

    std::optional<Compiler::Lsp::OutlineSymbol> outline_from_function(const AST::FunctionDeclNode &fn)
    {
        if (!fn.name_token.has_value()) {
            return std::nullopt;
        }

        Compiler::Lsp::OutlineSymbol symbol;
        symbol.name = fn.func_name();
        symbol.kind = function_outline_kind(fn);
        symbol.selection = AST::span_of(fn.name_token.value());
        symbol.range = span_through_scope(symbol.selection, fn.body);
        return symbol;
    }

    Compiler::Lsp::OutlineSymbol outline_from_property(const AST::VarDeclNode &prop)
    {
        Compiler::Lsp::OutlineSymbol symbol;
        symbol.name = prop.name_full();
        symbol.kind = Compiler::Lsp::OutlineKind::t_property;
        symbol.range = AST::span_of(prop.token_varname);
        symbol.selection = symbol.range;
        return symbol;
    }

    void add_function_child(
        Compiler::Lsp::OutlineSymbol &owner,
        const AST::FunctionDeclNode *fn
    )
    {
        if (fn == nullptr || skip_generated_function(*fn)) {
            return;
        }

        if (auto child = outline_from_function(*fn)) {
            owner.range = AST::union_span(owner.range, child->range);
            owner.children.push_back(std::move(child.value()));
        }
    }

    void add_property_child(Compiler::Lsp::OutlineSymbol &owner, const AST::VarDeclNode *prop)
    {
        if (prop == nullptr || !prop->token_varname.is_valid()) {
            return;
        }

        Compiler::Lsp::OutlineSymbol child = outline_from_property(*prop);
        owner.range = AST::union_span(owner.range, child.range);
        owner.children.push_back(std::move(child));
    }

    std::optional<Compiler::Lsp::OutlineSymbol> outline_from_type(const AST::TypeDeclNode &type)
    {
        if (!type.name_token.has_value()) {
            return std::nullopt;
        }

        Compiler::Lsp::OutlineSymbol symbol;
        symbol.name = type.type_name();
        symbol.kind = type_outline_kind(type);
        symbol.range = AST::span_of(type.name_token.value());
        symbol.selection = symbol.range;

        for (AST::VarDeclNode *prop : type.properties()) {
            add_property_child(symbol, prop);
        }

        for (AST::VarDeclNode *prop : type.complex_type().static_properties()) {
            add_property_child(symbol, prop);
        }

        for (AST::FunctionDeclNode *ctor : type.constructors()) {
            add_function_child(symbol, ctor);
        }

        for (AST::FunctionDeclNode *method : type.methods()) {
            add_function_child(symbol, method);
        }

        for (AST::FunctionDeclNode *method : type.complex_type().static_methods()) {
            add_function_child(symbol, method);
        }

        return symbol;
    }

    void add_outline_child(
        std::vector<Compiler::Lsp::OutlineSymbol> &out,
        Compiler::Lsp::OutlineSymbol *current_ns,
        Compiler::Lsp::OutlineSymbol symbol
    )
    {
        if (current_ns != nullptr) {
            current_ns->range = AST::union_span(current_ns->range, symbol.range);
            current_ns->children.push_back(std::move(symbol));
            return;
        }

        out.push_back(std::move(symbol));
    }

    std::string folded_query(const std::string &text)
    {
        std::string out = text;
        for (char &ch : out) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        return out;
    }

    bool name_matches_query(const std::string &name, const std::string &query)
    {
        if (query.empty()) {
            return true;
        }

        return folded_query(name).find(query) != std::string::npos;
    }

    void collect_workspace_symbols(
        const Compiler::Lsp::OutlineSymbol &symbol,
        const std::filesystem::path &path,
        const std::string &container,
        const std::string &query,
        std::vector<Compiler::Lsp::WorkspaceSymbol> &out
    )
    {
        if (name_matches_query(symbol.name, query)) {
            Compiler::Lsp::WorkspaceSymbol item;
            item.name = symbol.name;
            item.kind = symbol.kind;
            item.path = path;
            item.range = symbol.selection.file != nullptr ? symbol.selection : symbol.range;
            item.container = container;
            out.push_back(std::move(item));
        }

        for (const Compiler::Lsp::OutlineSymbol &child : symbol.children) {
            collect_workspace_symbols(child, path, symbol.name, query, out);
        }
    }

    std::string parameter_label(const AST::VarDeclNode &arg)
    {
        std::string label;
        const AST::AccessEffect effect = AST::declared_access_effect(arg);
        if (effect != AST::AccessEffect::t_none) {
            label += AST::access_effect_spelling(effect);
            label += " ";
        }

        if (arg.has_type()) {
            label += arg.type().get_type_desciption();
        }

        if (!arg.name_full().empty()) {
            if (!label.empty()) {
                label += " ";
            }

            label += arg.name_full();
        }

        return label;
    }

    Compiler::Lsp::SignatureHelp signature_help_of(const AST::FunctionDeclNode &decl)
    {
        Compiler::Lsp::SignatureHelp help;
        std::string prefix = decl.signature_description();
        const size_t paren = prefix.find('(');
        if (paren != std::string::npos) {
            prefix = prefix.substr(0, paren);
        }

        help.parameters.reserve(decl.args.size());
        std::string inside;
        for (size_t i = decl.implicit_arg_count(); i < decl.args.size(); i++) {
            const std::string label = parameter_label(*decl.args[i]);
            if (!inside.empty()) {
                inside += ", ";
            }

            inside += label;
            help.parameters.push_back(label);
        }

        help.label = prefix + "(" + inside + ")";
        return help;
    }

    AST::Span argument_span(const AST::ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        const TokenReference *token = AST::source_token_of(*expr);
        if (token == nullptr || !token->is_valid()) {
            return {};
        }

        return AST::span_of(*token);
    }

    uint32_t active_parameter_of(
        const AST::FunctionCallExprNode &call,
        AST::Location location
    )
    {
        const size_t implicit = call.decl != nullptr ? call.decl->implicit_arg_count() : 0;
        uint32_t active = 0;

        for (size_t i = implicit; i < call.arguments.size(); i++) {
            const AST::Span span = argument_span(call.arguments[i]);
            if (span.file == nullptr) {
                continue;
            }

            if (!AST::location_before(location, span.end)) {
                active = static_cast<uint32_t>(i - implicit + 1);
            }
        }

        const uint32_t last = call.decl != nullptr
            ? static_cast<uint32_t>(call.decl->args.size() - implicit)
            : static_cast<uint32_t>(call.arguments.size() > implicit
                ? call.arguments.size() - implicit
                : 0);

        if (last == 0) {
            return 0;
        }

        if (active >= last) {
            return last - 1;
        }

        return active;
    }
};

std::optional<Compiler::Lsp::HoverAnswer> Compiler::Lsp::hover(
    const Snapshot &snapshot,
    const AST::File &file,
    AST::Location location
)
{
    const PositionIndex::Entry *hit = snapshot.index.entry_at(&file, location.line, location.column);
    if (hit == nullptr || hit->node == nullptr) {
        return std::nullopt;
    }

    std::optional<HoverAnswer> answer = hover_of(hit->node, *snapshot.bundle);
    if (!answer.has_value()) {
        return std::nullopt;
    }

    answer->range.file = &file;
    answer->range.start = AST::Location{ hit->line, hit->column };
    answer->range.end = AST::Location{ hit->line, hit->column + (hit->width > 0 ? hit->width : 1) };
    return answer;
}

std::optional<Compiler::Lsp::DefinitionAnswer> Compiler::Lsp::definition(
    const Snapshot &snapshot,
    const AST::File &file,
    AST::Location location
)
{
    const PositionIndex::Entry *hit = snapshot.index.entry_at(&file, location.line, location.column);
    if (hit == nullptr || hit->node == nullptr) {
        return std::nullopt;
    }

    std::unordered_map<AST::Node *, AST::Node *> cache;
    AST::Node *target = target_of(hit->node, *snapshot.bundle, cache);
    if (target == nullptr) {
        return std::nullopt;
    }

    const TokenReference *token = name_token_of(target);
    if (token == nullptr || !token->is_valid() || token->file() == nullptr) {
        return std::nullopt;
    }

    const std::filesystem::path dest = Compiler::canonical_or_absolute(token->file()->get_path());
    if (is_embedded_stdlib_path(dest)) {
        return std::nullopt;
    }

    DefinitionAnswer answer;
    answer.path = dest;
    answer.range = AST::span_of(*token);
    return answer;
}

std::vector<Compiler::Lsp::OutlineSymbol> Compiler::Lsp::document_symbols(const AST::File &file)
{
    std::vector<OutlineSymbol> out;

    if (file.root == nullptr) {
        return out;
    }

    OutlineSymbol *current_ns = nullptr;

    // `namespace x;` is not a child of the file root; it only lives in the arena
    if (file.module != nullptr) {
        for (AST::NamespaceDeclNode *ns : file.module->nodes.of_type<AST::NamespaceDeclNode>()) {
            if (!ns->namespace_tokens.start_ref().is_valid()
                || ns->namespace_tokens.start_ref().file() != &file) {
                continue;
            }

            OutlineSymbol symbol;
            symbol.name = ns->namespace_decl != nullptr
                ? ns->namespace_decl->display_name()
                : std::string("namespace");
            symbol.kind = OutlineKind::t_namespace;
            symbol.range = AST::span_of(ns->namespace_tokens.start_ref());
            symbol.selection = symbol.range;

            bool already = false;
            for (const OutlineSymbol &existing : out) {
                if (existing.kind == OutlineKind::t_namespace && existing.name == symbol.name) {
                    already = true;
                    break;
                }
            }

            if (already) {
                continue;
            }

            out.push_back(std::move(symbol));
            current_ns = &out.back();
        }
    }

    auto nest = [&](OutlineSymbol symbol) {
        add_outline_child(out, current_ns, std::move(symbol));
    };

    for (const AST::NodeReference &child : file.root->children) {
        const AST::NodeReference ref = child;

        if (ref.has_type<AST::NamespaceDeclNode>()) {
            AST::NamespaceDeclNode *ns = ref.get_ptr<AST::NamespaceDeclNode>();
            if (!ns->namespace_tokens.start_ref().is_valid()) {
                continue;
            }

            OutlineSymbol symbol;
            symbol.name = ns->namespace_decl != nullptr
                ? ns->namespace_decl->display_name()
                : std::string("namespace");
            symbol.kind = OutlineKind::t_namespace;
            symbol.range = AST::span_of(ns->namespace_tokens.start_ref());
            symbol.selection = symbol.range;

            bool already = false;
            for (const OutlineSymbol &existing : out) {
                if (existing.kind == OutlineKind::t_namespace && existing.name == symbol.name) {
                    already = true;
                    break;
                }
            }

            if (already) {
                continue;
            }

            out.push_back(std::move(symbol));
            current_ns = &out.back();
            continue;
        }

        if (ref.has_type<AST::TypeDeclNode>()) {
            AST::TypeDeclNode *type = ref.get_ptr<AST::TypeDeclNode>();
            if (!type->name_token.has_value() || type->name_token.value().file() != &file) {
                continue;
            }

            if (auto symbol = outline_from_type(*type)) {
                nest(std::move(symbol.value()));
            }

            continue;
        }

        if (ref.has_type<AST::FunctionDeclNode>()) {
            AST::FunctionDeclNode *fn = ref.get_ptr<AST::FunctionDeclNode>();
            if (skip_from_file_outline(*fn) || !fn->name_token.has_value()
                || fn->name_token.value().file() != &file) {
                continue;
            }

            if (auto symbol = outline_from_function(*fn)) {
                nest(std::move(symbol.value()));
            }

            continue;
        }

        if (ref.has_type<AST::ConstDeclNode>()) {
            AST::ConstDeclNode *decl = ref.get_ptr<AST::ConstDeclNode>();
            OutlineSymbol symbol;
            symbol.name = decl->name();
            symbol.kind = OutlineKind::t_constant;
            symbol.range = AST::span_of(decl->token_name);
            symbol.selection = symbol.range;
            nest(std::move(symbol));
        }
    }

    // constants live in the arena, not the file root
    if (file.module != nullptr) {
        for (AST::ConstDeclNode *decl : file.module->nodes.of_type<AST::ConstDeclNode>()) {
            if (decl->declared_in.file != &file) {
                continue;
            }

            bool already = false;
            auto contains = [&](const std::vector<OutlineSymbol> &symbols) {
                for (const OutlineSymbol &symbol : symbols) {
                    if (symbol.kind == OutlineKind::t_constant && symbol.name == decl->name()) {
                        return true;
                    }
                }

                return false;
            };

            already = contains(out);
            if (!already && current_ns != nullptr) {
                already = contains(current_ns->children);
            }

            if (already) {
                continue;
            }

            OutlineSymbol symbol;
            symbol.name = decl->name();
            symbol.kind = OutlineKind::t_constant;
            symbol.range = AST::span_of(decl->token_name);
            symbol.selection = symbol.range;
            nest(std::move(symbol));
        }
    }

    return out;
}

std::vector<Compiler::Lsp::DefinitionAnswer> Compiler::Lsp::references(
    const Snapshot &snapshot,
    const AST::File &file,
    AST::Location location,
    bool include_declaration
)
{
    std::vector<DefinitionAnswer> out;

    const PositionIndex::Entry *hit = snapshot.index.entry_at(&file, location.line, location.column);
    if (hit == nullptr || hit->node == nullptr) {
        return out;
    }

    std::unordered_map<AST::Node *, AST::Node *> cache;
    AST::Node *wanted = target_of(hit->node, *snapshot.bundle, cache);
    if (wanted == nullptr) {
        return out;
    }
    const TokenReference *decl_token = name_token_of(wanted);
    std::unordered_set<std::string> seen;

    auto push = [&](const AST::File &from, const AST::Span &span) {
        if (span.file == nullptr || is_embedded_stdlib_path(from.get_path())) {
            return;
        }

        const std::string key = from.get_path().string()
            + ":" + std::to_string(span.start.line)
            + ":" + std::to_string(span.start.column);
        if (!seen.insert(key).second) {
            return;
        }

        DefinitionAnswer answer;
        answer.path = from.get_path();
        answer.range = span;
        out.push_back(std::move(answer));
    };

    if (include_declaration && decl_token != nullptr && decl_token->is_valid()
        && decl_token->file() != nullptr) {
        push(*decl_token->file(), AST::span_of(*decl_token));
    }

    snapshot.index.visit_entries(
        [&](const AST::File &indexed, const PositionIndex::Entry &entry) {
            if (entry.node == nullptr || target_of(entry.node, *snapshot.bundle, cache) != wanted) {
                return;
            }

            if (entry_is_declaration(entry, wanted) && !include_declaration) {
                return;
            }

            AST::Span span;
            span.file = &indexed;
            span.start = AST::Location{ entry.line, entry.column };
            span.end = AST::Location{ entry.line, entry.column + (entry.width > 0 ? entry.width : 1) };
            push(indexed, span);
        });

    return out;
}

std::vector<Compiler::Lsp::WorkspaceSymbol> Compiler::Lsp::workspace_symbols(
    const Snapshot &snapshot,
    const std::string &query
)
{
    std::vector<WorkspaceSymbol> out;
    const std::string folded = folded_query(query);
    for (const AST::File *file : snapshot.index.files()) {
        if (file == nullptr || is_embedded_stdlib_path(file->get_path())) {
            continue;
        }

        for (const OutlineSymbol &symbol : document_symbols(*file)) {
            collect_workspace_symbols(symbol, file->get_path(), "", folded, out);
        }
    }

    return out;
}

std::optional<Compiler::Lsp::SignatureHelp> Compiler::Lsp::signature_help(
    const Snapshot &snapshot,
    const AST::File &file,
    AST::Location location
)
{
    AST::FunctionCallExprNode *call = snapshot.index.enclosing_call(
        &file, location.line, location.column);
    if (call == nullptr || call->decl == nullptr) {
        return std::nullopt;
    }

    SignatureHelp help = signature_help_of(*call->decl);
    help.active_parameter = active_parameter_of(*call, location);
    if (!help.parameters.empty() && help.active_parameter >= help.parameters.size()) {
        help.active_parameter = static_cast<uint32_t>(help.parameters.size() - 1);
    }

    return help;
}
