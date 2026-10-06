#include "Compiler/Lsp/LspCompletionLookup.h"

#include "AST/ASTBundle.h"
#include "AST/ASTConstness.h"
#include "AST/ASTFile.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTImport.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTSymbol.h"
#include "AST/ASTTypeUnify.h"
#include "AST/FunctionDeclNode.h"
#include "AST/NamespaceDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"

const AST::ComplexType *Compiler::Lsp::complex_of(const AST::ValueType &type)
{
    AST::ValueType target = AST::target_type_of(type);
    if (target.is_wrapped_optional()) {
        target = AST::target_type_of(target.optional_payload());
    }

    return target.has_complex_type() ? target.get_complex_type() : nullptr;
}

std::vector<AST::FunctionDeclNode *> Compiler::Lsp::methods_of(const AST::ComplexType &type)
{
    return type.template_or_self()->methods();
}

Compiler::Lsp::CompletionLookup::CompletionLookup(const CompletionRequest &request) :
    _request(request)
{
    if (_request.live == nullptr) {
        return;
    }

    _context = analyze_completion_context(_request.live->text(), _request.cursor);
    settle_scope();
}

// the namespace, origin, and enclosing type at the cursor, from the snapshot
void Compiler::Lsp::CompletionLookup::settle_scope()
{
    if (_request.snapshot == nullptr || _request.file == nullptr) {
        return;
    }

    _bundle = _request.snapshot->bundle.get();
    _origin.module = _request.file->module;
    _origin.file = _request.file;
    _namespace = namespace_at_cursor();

    if (const LexicalFrame *type = enclosing_type_frame(_context)) {
        if (AST::TypeDeclNode *decl = type_decl_named_at(type->name_offset, type->name)) {
            _enclosing = &decl->complex_type();
        }
    }
}

const AST::Namespace *Compiler::Lsp::CompletionLookup::namespace_at_cursor()
{
    const AST::Namespace *found = &_bundle->collector.namespaces.root();
    if (_request.file->module == nullptr) {
        return found;
    }

    const AST::Location cursor = _request.live->location_of(_request.cursor);
    const uint32_t line = _request.map.to_snapshot(cursor.line).value_or(cursor.line);
    uint32_t best = 0;

    for (AST::NamespaceDeclNode *decl : _request.file->module->nodes.of_type<AST::NamespaceDeclNode>()) {
        const TokenReference start = decl->namespace_tokens.start_ref();
        if (!start.is_valid() || start.file() != _request.file || decl->namespace_decl == nullptr) {
            continue;
        }

        if (start.line() <= line && start.line() >= best) {
            best = start.line();
            found = decl->namespace_decl;
        }
    }

    return found;
}

AST::Node *Compiler::Lsp::CompletionLookup::node_at(size_t offset) const
{
    if (_bundle == nullptr) {
        return nullptr;
    }

    const auto location = _request.map.location_to_snapshot(_request.live->location_of(offset));
    if (!location.has_value()) {
        return nullptr;
    }

    return _request.snapshot->index.at(_request.file, location->line, location->column);
}

AST::TypeDeclNode *Compiler::Lsp::CompletionLookup::type_decl_named_at(
    size_t offset,
    const std::string &name
) const
{
    const AST::NodeReference ref = AST::make_ref(node_at(offset));
    if (ref.node() != nullptr && ref.has_type<AST::TypeDeclNode>()) {
        return ref.get_ptr<AST::TypeDeclNode>();
    }

    return resolve_type_path({ name });
}

const AST::FunctionDeclNode *Compiler::Lsp::CompletionLookup::enclosing_function() const
{
    const LexicalFrame *frame = enclosing_function_frame(_context);
    if (frame == nullptr || frame->name.empty()) {
        return nullptr;
    }

    const AST::NodeReference ref = AST::make_ref(node_at(frame->name_offset));
    if (ref.node() != nullptr && ref.has_type<AST::FunctionDeclNode>()) {
        return ref.get_ptr<AST::FunctionDeclNode>();
    }

    if (_enclosing != nullptr) {
        for (const auto *members : { &_enclosing->methods(), &_enclosing->static_methods() }) {
            for (AST::FunctionDeclNode *fn : *members) {
                if (fn->func_name() == frame->name) {
                    return fn;
                }
            }
        }
        return nullptr;
    }

    return _namespace != nullptr ? first_overload(frame->name, *_namespace) : nullptr;
}

bool Compiler::Lsp::CompletionLookup::visible(
    AST::Visibility visibility,
    const AST::DeclarationOrigin &origin,
    const AST::ComplexType *owner
) const
{
    if (visibility == AST::Visibility::t_owner) {
        return owner == nullptr || AST::can_reach_private_member(_enclosing, owner);
    }

    if (!_origin.is_known() || !origin.is_known()) {
        return true;
    }

    return AST::visible_from(visibility, origin, _origin);
}

bool Compiler::Lsp::CompletionLookup::visible(const AST::FunctionDeclNode &fn) const
{
    return visible(fn.visibility, fn.declared_in, fn.owner_type);
}

const AST::Namespace *Compiler::Lsp::CompletionLookup::child_namespace(
    const AST::Namespace &parent,
    const std::vector<std::string> &segments
) const
{
    if (_bundle == nullptr) {
        return nullptr;
    }

    const AST::Namespace *at = &parent;
    for (const std::string &segment : segments) {
        at = _bundle->collector.namespaces.get(*at, segment);
        if (at == nullptr) {
            return nullptr;
        }
    }

    return at;
}

const AST::ImportBinding *Compiler::Lsp::CompletionLookup::import_named(const std::string &name) const
{
    if (_bundle == nullptr || _request.file == nullptr) {
        return nullptr;
    }

    return AST::file_import_for(*_request.file, _bundle->collector, name);
}

const AST::Namespace *Compiler::Lsp::CompletionLookup::resolve_namespace_path(
    const std::vector<std::string> &path
) const
{
    if (_bundle == nullptr || path.empty()) {
        return nullptr;
    }

    const std::vector<std::string> rest(path.begin() + 1, path.end());
    if (const AST::ImportBinding *binding = import_named(path[0])) {
        if (binding->kind == AST::ImportKind::t_namespace) {
            return child_namespace(*binding->target_namespace, rest);
        }

        if (const AST::Namespace *item = _bundle->collector.namespaces.get(
            *binding->target_namespace,
            binding->target_name
        )) {
            return child_namespace(*item, rest);
        }
    }

    for (const AST::Namespace *from = _namespace; from != nullptr; from = from->parent()) {
        if (const AST::Namespace *found = child_namespace(*from, path)) {
            return found;
        }
    }

    return child_namespace(_bundle->collector.namespaces.root(), path);
}

AST::TypeDeclNode *Compiler::Lsp::CompletionLookup::type_of_symbol(const AST::Symbol *symbol) const
{
    if (symbol == nullptr || symbol->type() != AST::SymbolType::t_type
        || !symbol->node.has_type<AST::TypeDeclNode>()) {
        return nullptr;
    }

    return symbol->node.get_ptr<AST::TypeDeclNode>();
}

AST::TypeDeclNode *Compiler::Lsp::CompletionLookup::resolve_type_path(
    const std::vector<std::string> &path
) const
{
    if (_bundle == nullptr || path.empty()) {
        return nullptr;
    }

    if (path.size() == 1 && path[0] == "self") {
        if (const LexicalFrame *type = enclosing_type_frame(_context)) {
            return type_decl_named_at(type->name_offset, type->name);
        }
        return nullptr;
    }

    AST::NamespaceManager &namespaces = _bundle->collector.namespaces;
    const std::string &last = path.back();

    if (path.size() == 1) {
        if (const AST::ImportBinding *binding = import_named(last)) {
            if (binding->kind == AST::ImportKind::t_item) {
                return type_of_symbol(namespaces.find_symbol(
                    binding->target_name, *binding->target_namespace));
            }
        }

        return type_of_symbol(namespaces.find_symbol_in_scope(last, *_namespace));
    }

    const std::vector<std::string> prefix(path.begin(), path.end() - 1);
    if (const AST::Namespace *ns = resolve_namespace_path(prefix)) {
        if (AST::TypeDeclNode *found = type_of_symbol(namespaces.find_symbol(last, *ns))) {
            return found;
        }
    }

    if (AST::TypeDeclNode *owner = resolve_type_path(prefix)) {
        return owner->complex_type().find_member_type_decl(last);
    }

    return nullptr;
}

AST::TypeDeclNode *Compiler::Lsp::CompletionLookup::resolve_type_text(std::string text) const
{
    if (text.rfind("const", 0) == 0) {
        text = text.substr(5);
    }

    const size_t generic = text.find('<');
    if (generic != std::string::npos) {
        text = text.substr(0, generic);
    }

    while (!text.empty() && (text.back() == '?' || text.back() == '&' || text.back() == '*'
        || text.back() == ']' || text.back() == '[')) {
        text.pop_back();
    }

    std::vector<std::string> path;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t sep = text.find("::", start);
        path.push_back(text.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos) {
            break;
        }
        start = sep + 2;
    }

    return resolve_type_path(path);
}

const AST::FunctionDeclNode *Compiler::Lsp::CompletionLookup::first_overload(
    const std::string &name,
    const AST::Namespace &ns
) const
{
    if (_bundle == nullptr) {
        return nullptr;
    }

    for (AST::FunctionDeclNode *fn : _bundle->collector.functions.overloads(name, ns)) {
        if (fn != nullptr && !fn->is_member()) {
            return fn;
        }
    }

    return nullptr;
}

std::optional<AST::ValueType> Compiler::Lsp::CompletionLookup::type_of_lexical(
    const LexicalVariable &variable
)
{
    const AST::NodeReference ref = AST::make_ref(node_at(variable.offset));
    if (ref.node() != nullptr) {
        if (ref.has_type<AST::VarDeclNode>() && ref.get_ptr<AST::VarDeclNode>()->has_type()) {
            return ref.get_ptr<AST::VarDeclNode>()->type();
        }

        if (ref.has_type<AST::VarNode>() && ref.get_ptr<AST::VarNode>()->decl().has_type()) {
            return ref.get_ptr<AST::VarNode>()->decl().type();
        }
    }

    if (!variable.written_type.empty()) {
        if (AST::TypeDeclNode *type = resolve_type_text(variable.written_type)) {
            return type->value_type();
        }
        return std::nullopt;
    }

    if (variable.initializer.empty() || _depth >= 4) {
        return std::nullopt;
    }

    _depth++;
    const auto type = type_of_chain(receiver_chain_of(variable.initializer));
    _depth--;
    return type;
}

std::optional<AST::ValueType> Compiler::Lsp::CompletionLookup::type_of_variable(const std::string &name)
{
    if (name == "$this") {
        const LexicalFrame *owner = this_frame(_context);
        if (owner == nullptr) {
            return std::nullopt;
        }

        if (AST::TypeDeclNode *type = type_decl_named_at(owner->name_offset, owner->name)) {
            return type->value_type();
        }
        return std::nullopt;
    }

    for (const LexicalVariable &variable : visible_variables(_context)) {
        if (variable.name == name) {
            return type_of_lexical(variable);
        }
    }

    return std::nullopt;
}

std::optional<AST::ValueType> Compiler::Lsp::CompletionLookup::type_of_root(const ReceiverSegment &root)
{
    switch (root.kind) {
    case ReceiverSegment::Kind::t_variable:
        return type_of_variable(root.name);

    case ReceiverSegment::Kind::t_call: {
        if (AST::TypeDeclNode *type = resolve_type_path({ root.name })) {
            return type->value_type();
        }

        const AST::Namespace *ns = _namespace;
        std::string name = root.name;
        if (_request.file != nullptr) {
            AST::apply_item_import(*_request.file, _bundle->collector, name, ns, name);
        }

        if (ns != nullptr) {
            if (const AST::FunctionDeclNode *fn = first_overload(name, *ns)) {
                return fn->get_return_type();
            }
        }
        return std::nullopt;
    }

    case ReceiverSegment::Kind::t_static_call: {
        if (AST::TypeDeclNode *type = resolve_type_path(root.path)) {
            for (AST::FunctionDeclNode *fn : AST::find_static_functions(&type->complex_type(), root.name)) {
                return fn->get_return_type();
            }
        }

        if (const AST::Namespace *ns = resolve_namespace_path(root.path)) {
            if (const AST::FunctionDeclNode *fn = first_overload(root.name, *ns)) {
                return fn->get_return_type();
            }
        }
        return std::nullopt;
    }

    case ReceiverSegment::Kind::t_static_value: {
        AST::TypeDeclNode *type = resolve_type_path(root.path);
        if (type != nullptr && type->complex_type().find_enum_case(root.name) != nullptr) {
            return type->value_type();
        }
        return std::nullopt;
    }

    default:
        return std::nullopt;
    }
}

std::optional<AST::ValueType> Compiler::Lsp::CompletionLookup::type_of_index(
    const AST::ValueType &current
) const
{
    AST::ValueType base = current;
    while (base.is_pointer() && !base.is_nullable()) {
        base = base.pointee();
    }

    if (base.is_pointer()) {
        return base.pointee();
    }

    if (base.is_inline_array()) {
        return base.array_element();
    }

    const AST::ComplexType *type = complex_of(base);
    if (type == nullptr) {
        return std::nullopt;
    }

    const AST::ComplexType *wanted = type->template_or_self();
    for (AST::FunctionDeclNode *fn : _bundle->collector.functions.overloads(
        AST::index_operator_name(), _bundle->collector.namespaces.root())) {
        if (fn == nullptr || fn->args.size() < 2) {
            continue;
        }

        const AST::ValueType declared = AST::operator_receiver_type(*fn);
        if (!declared.has_complex_type()
            || declared.get_complex_type()->template_or_self() != wanted) {
            continue;
        }

        AST::TypeSubstitution subst;
        if (!AST::unify_type(declared, AST::value_type_of(current), subst)) {
            continue;
        }

        return AST::substitute_type(fn->get_return_type(), subst, _bundle->collector.type_registry);
    }

    return std::nullopt;
}

std::optional<AST::ValueType> Compiler::Lsp::CompletionLookup::type_of_chain(
    const std::vector<ReceiverSegment> &chain,
    bool optional
)
{
    if (chain.empty() || _bundle == nullptr) {
        return std::nullopt;
    }

    std::optional<AST::ValueType> current = type_of_root(chain[0]);
    for (size_t i = 1; i < chain.size() && current.has_value(); i++) {
        const ReceiverSegment &segment = chain[i];
        if (segment.kind == ReceiverSegment::Kind::t_index) {
            current = type_of_index(current.value());
            continue;
        }

        const AST::ComplexType *type = complex_of(current.value());
        if (type == nullptr) {
            return std::nullopt;
        }

        current.reset();

        if (segment.kind == ReceiverSegment::Kind::t_property) {
            if (const AST::ComplexType::Property *property = type->find_property(segment.name)) {
                current = property->type;
            }
        }
        else if (segment.kind == ReceiverSegment::Kind::t_method) {
            for (AST::FunctionDeclNode *fn : AST::find_member_functions(type, segment.name)) {
                current = fn->get_return_type();
                break;
            }
        }
    }

    if (optional && current.has_value() && current->is_nullable()) {
        current = AST::ValueType::make_non_nullable(current.value());
    }

    return current;
}
