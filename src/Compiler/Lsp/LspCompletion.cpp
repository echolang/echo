#include "Compiler/Lsp/LspCompletion.h"

#include "AST/ASTAttributes.h"
#include "AST/ASTBundle.h"
#include "AST/ASTFile.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTImport.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"
#include "AST/ASTValueType.h"
#include "AST/ASTVisibility.h"
#include "AST/ConstDeclNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "Compiler/Lsp/LspCompletionLookup.h"
#include "Compiler/Lsp/LspResolve.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_set>

namespace
{
    using Compiler::Lsp::CompletionContext;
    using Compiler::Lsp::CompletionItem;
    using Compiler::Lsp::CompletionItemKind;
    using Compiler::Lsp::CompletionKind;
    using Compiler::Lsp::LexicalVariable;
    using Compiler::Lsp::NamePosition;
    using Compiler::Lsp::ReceiverSegment;

    // the first character of a sort text. nearer first: the cursor's own scope, then this file
    // and the receiver's own type, then this module, then everything else (the standard library),
    // then keywords
    constexpr char k_bucket_local = '0';
    constexpr char k_bucket_own = '1';
    constexpr char k_bucket_module = '2';
    constexpr char k_bucket_other = '3';
    constexpr char k_bucket_keyword = '4';

    const std::vector<std::string> &keywords_for(NamePosition position)
    {
        static const std::vector<std::string> file_root = {
            "function", "struct", "class", "enum", "interface", "test", "namespace", "use", "extern",
            "public", "private", "internal", "static", "const", "if", "while", "for", "foreach", "match",
            "echo", "unsafe" };
        static const std::vector<std::string> type_body = {
            "function", "static", "const", "operator", "public", "private", "internal", "constructor",
            "destructor", "init", "case", "map", "type" };
        static const std::vector<std::string> statement = {
            "if", "else", "while", "for", "foreach", "return", "echo", "break", "continue", "unsafe",
            "match", "const", "guard", "mv", "function", "true", "false", "null" };
        static const std::vector<std::string> expression = {
            "true", "false", "null", "match", "mv", "function", "guard" };
        static const std::vector<std::string> after_expression = { "as", "instanceof" };
        static const std::vector<std::string> type = { "ptr", "weak", "function", "const" };

        switch (position) {
        case NamePosition::t_file_root:
            return file_root;
        case NamePosition::t_type_body:
            return type_body;
        case NamePosition::t_statement:
            return statement;
        case NamePosition::t_expression:
            return expression;
        case NamePosition::t_after_expression:
            return after_expression;
        case NamePosition::t_type:
            return type;
        }

        return expression;
    }

    std::vector<std::string> primitive_names()
    {
        std::vector<std::string> out;
        for (int p = static_cast<int>(AST::ValueTypePrimitive::t_int8);
            p <= static_cast<int>(AST::ValueTypePrimitive::t_void); p++) {
            out.push_back(AST::get_primitive_name(static_cast<AST::ValueTypePrimitive>(p)));
        }

        return out;
    }

    char folded_char(char c)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    std::string without_sigil(const std::string &name)
    {
        return !name.empty() && name[0] == '$' ? name.substr(1) : name;
    }

    CompletionItemKind kind_of_type(const AST::TypeDeclNode &type)
    {
        switch (type.kind()) {
        case AST::ComplexTypeKind::t_class:
            return CompletionItemKind::t_class;
        case AST::ComplexTypeKind::t_interface:
            return CompletionItemKind::t_interface;
        case AST::ComplexTypeKind::t_enum:
            return CompletionItemKind::t_enum;
        case AST::ComplexTypeKind::t_struct:
        case AST::ComplexTypeKind::t_opaque:
            return CompletionItemKind::t_struct;
        }

        return CompletionItemKind::t_struct;
    }

    // a name the user can type. synthesized, unspellable, and syntax-only callees stay out
    bool is_completable(const AST::FunctionDeclNode &fn)
    {
        if (fn.is_instantiated() || fn.is_closure || fn.is_test() || fn.is_operator() || fn.is_constructor()
            || fn.is_destructor() || fn.is_init()) {
            return false;
        }

        // a payload case already completes as the case; the synthesized constructor is the same name
        if (fn.is_static_method() && fn.owner_type != nullptr
            && fn.owner_type->find_enum_case(fn.func_name()) != nullptr) {
            return false;
        }

        if (fn.is_implicitly_generated && (!fn.name_token.has_value() || fn.name_token->is_minted())) {
            return false;
        }

        const std::string name = fn.func_name();
        return !name.empty() && name[0] != '_' && name[0] != '$' && name[0] != '[';
    }

    // the parameters as signature help spells them: `(int32 $a, forEvent: string $name)`
    std::string parameter_list_of(const AST::FunctionDeclNode &fn)
    {
        const std::vector<std::string> spellings = fn.written_parameter_spellings();
        const size_t implicit = fn.implicit_arg_count();
        std::string out = "(";
        for (size_t i = 0; i < spellings.size(); i++) {
            std::string label = spellings[i];
            const AST::VarDeclNode *arg = implicit + i < fn.args.size() ? fn.args[implicit + i] : nullptr;
            if (arg != nullptr && label.find('$') == std::string::npos && !arg->name_full().empty()) {
                label += " " + arg->name_full();
            }

            out += (i > 0 ? ", " : "") + label;
        }

        return out + ")";
    }

    using Compiler::Lsp::complex_of;
    using Compiler::Lsp::methods_of;

    class Completer
    {
    public:

        explicit Completer(const Compiler::Lsp::CompletionRequest &request) :
            _request(request),
            _lookup(request),
            _context(_lookup.context())
        {}

        Compiler::Lsp::CompletionAnswer run()
        {
            Compiler::Lsp::CompletionAnswer answer;
            if (_request.live == nullptr) {
                return answer;
            }

            answer.replace_start = _context.replace_start;
            answer.replace_end = _context.replace_end;

            // a trigger character that started nothing: `$a >`, `f($a:`, `3.`
            if (_request.from_trigger_character && _context.kind == CompletionKind::t_identifier
                && _context.prefix.empty()) {
                return answer;
            }

            switch (_context.kind) {
            case CompletionKind::t_none:
                break;
            case CompletionKind::t_variable:
                add_variables();
                break;
            case CompletionKind::t_member:
                if (const auto type = _lookup.type_of_chain(_context.receiver, _context.optional_chain)) {
                    add_members(type.value());
                }
                break;
            case CompletionKind::t_static:
                add_static_path();
                break;
            case CompletionKind::t_use_path:
                if (const AST::Namespace *ns = _lookup.resolve_namespace_path(_context.path)) {
                    add_namespace(*ns, false);
                }
                break;
            case CompletionKind::t_shorthand:
                add_shorthand();
                break;
            case CompletionKind::t_attribute:
                add_attributes();
                break;
            case CompletionKind::t_identifier:
                _filter_first_character = !_context.prefix.empty();
                add_identifiers();
                break;
            }

            answer.items = std::move(_items);
            return answer;
        }

    private:

        const Compiler::Lsp::CompletionRequest &_request;
        Compiler::Lsp::CompletionLookup _lookup;
        const CompletionContext &_context;
        std::vector<CompletionItem> _items;
        std::unordered_set<std::string> _seen;
        bool _filter_first_character = false;

        bool visible(
            AST::Visibility visibility,
            const AST::DeclarationOrigin &origin,
            const AST::ComplexType *owner
        ) const {
            return _lookup.visible(visibility, origin, owner);
        }

        bool visible(const AST::FunctionDeclNode &fn) const {
            return _lookup.visible(fn);
        }

        char bucket_of(const AST::DeclarationOrigin &origin) const
        {
            if (origin.file != nullptr && origin.file == _lookup.origin().file) {
                return k_bucket_own;
            }

            return origin.same_module(_lookup.origin()) ? k_bucket_module : k_bucket_other;
        }

        // ------------------------------------------------------------------------------------------
        // items

        void add(CompletionItem item, char bucket)
        {
            if (item.filter_text.empty()) {
                item.filter_text = item.label;
            }

            if (_filter_first_character) {
                const std::string typed = without_sigil(_context.prefix);
                const std::string offered = without_sigil(item.filter_text);
                if (!typed.empty() && (offered.empty() || folded_char(offered[0]) != folded_char(typed[0]))) {
                    return;
                }
            }

            if (!_seen.insert(item.label + "#" + std::to_string(static_cast<int>(item.kind))).second) {
                return;
            }

            if (item.insert_text.empty()) {
                item.insert_text = item.label;
            }

            item.sort_text = std::string(1, bucket) + item.sort_text + item.label;
            _items.push_back(std::move(item));
        }

        CompletionItem callable_item(const AST::FunctionDeclNode &fn, CompletionItemKind kind) const
        {
            CompletionItem item;
            item.label = fn.func_name();
            item.kind = kind;
            item.label_detail = parameter_list_of(fn);
            item.label_description = fn.get_return_type().get_type_desciption();
            item.detail = item.label + item.label_detail + " : " + item.label_description;
            if (_request.snippets) {
                item.insert_text = item.label + "($0)";
                item.is_snippet = true;
            }

            return item;
        }

        void add_type(const AST::TypeDeclNode &type, char bucket)
        {
            const AST::ComplexType &complex = type.complex_type();
            if (!visible(complex.visibility, complex.declared_in, nullptr)) {
                return;
            }

            CompletionItem item;
            item.label = type.type_name();
            item.kind = kind_of_type(type);
            item.detail = type.value_type().get_type_desciption();
            if (type.ast_namespace != nullptr && !type.ast_namespace->is_root()) {
                item.label_description = type.ast_namespace->display_name();
            }
            add(std::move(item), bucket);
        }

        void add_constant(const AST::ConstDeclNode &constant, char bucket)
        {
            if (!visible(constant.visibility, constant.declared_in, nullptr)) {
                return;
            }

            CompletionItem item;
            item.label = constant.name();
            item.kind = CompletionItemKind::t_constant;
            if (constant.value != nullptr) {
                item.label_description = constant.value->result_type().get_type_desciption();
                item.detail = "const " + item.label + " : " + item.label_description;
            }
            add(std::move(item), bucket);
        }

        void add_function(const AST::FunctionDeclNode &fn, CompletionItemKind kind, char bucket)
        {
            if (!is_completable(fn) || !visible(fn)) {
                return;
            }

            add(callable_item(fn, kind), bucket);
        }

        // ------------------------------------------------------------------------------------------
        // the answers

        void add_variables()
        {
            const std::vector<LexicalVariable> variables = Compiler::Lsp::visible_variables(_context);
            for (size_t i = 0; i < variables.size(); i++) {
                CompletionItem item;
                item.label = variables[i].name;
                item.kind = CompletionItemKind::t_variable;

                // nearest first: the index keeps the order visible_variables walked in
                item.sort_text = std::to_string(100 + i);
                if (const auto type = _lookup.type_of_lexical(variables[i])) {
                    item.label_description = type->get_type_desciption();
                }
                else if (!variables[i].written_type.empty()) {
                    item.label_description = variables[i].written_type;
                }
                item.detail = item.label_description;
                add(std::move(item), k_bucket_local);
            }

            if (const Compiler::Lsp::LexicalFrame *owner = Compiler::Lsp::this_frame(_context)) {
                CompletionItem item;
                item.label = "$this";
                item.kind = CompletionItemKind::t_variable;
                item.label_description = owner->name;
                item.detail = owner->name;
                add(std::move(item), k_bucket_local);
            }
        }

        void add_members(const AST::ValueType &type)
        {
            const AST::ComplexType *complex = complex_of(type);
            if (complex == nullptr) {
                return;
            }

            // an enum's properties are its cases' payloads, laid out side by side. `->` names
            // methods and user fields, so these stay out of the list
            if (!complex->is_enum_kind()) {
                for (size_t i = 0; i < complex->property_count(); i++) {
                    const AST::ComplexType::Property &property = complex->get_property(i);
                    if (property.name.empty() || property.name[0] == '_'
                        || !visible(property.visibility, complex->declared_in, complex)) {
                        continue;
                    }

                    CompletionItem item;
                    item.label = without_sigil(property.name);
                    item.kind = CompletionItemKind::t_field;
                    item.label_description = property.type.get_type_desciption();
                    item.detail = item.label_description;
                    add(std::move(item), k_bucket_own);
                }
            }

            for (AST::FunctionDeclNode *fn : methods_of(*complex)) {
                add_function(*fn, CompletionItemKind::t_method, k_bucket_own);
            }
        }

        void add_statics(const AST::TypeDeclNode &type, bool variables_only)
        {
            const AST::ComplexType &complex = type.complex_type();

            for (AST::VarDeclNode *property : complex.static_properties()) {
                if (property == nullptr || !visible(property->visibility, complex.declared_in, &complex)) {
                    continue;
                }

                CompletionItem item;
                item.label = property->name_full();
                item.kind = CompletionItemKind::t_property;
                if (property->has_type()) {
                    item.label_description = property->type().get_type_desciption();
                    item.detail = item.label_description;
                }
                add(std::move(item), k_bucket_own);
            }

            if (variables_only) {
                return;
            }

            for (const AST::ComplexType::EnumCase &entry : complex.enum_cases()) {
                CompletionItem item;
                item.label = entry.name;
                item.kind = CompletionItemKind::t_enum_member;
                item.label_description = type.type_name();
                item.detail = "case " + entry.name;
                if (entry.has_payload() && _request.snippets) {
                    item.insert_text = entry.name + "($0)";
                    item.is_snippet = true;
                }
                add(std::move(item), k_bucket_own);
            }

            for (AST::FunctionDeclNode *fn : complex.template_or_self()->static_methods()) {
                add_function(*fn, CompletionItemKind::t_method, k_bucket_own);
            }

            // what the body declared besides members: its constants and nested types, which live in
            // the namespace the type opens
            AST::Bundle *bundle = _lookup.bundle();
            if (bundle != nullptr && type.ast_namespace != nullptr) {
                if (const AST::Namespace *surface = bundle->collector.namespaces.get(
                    *type.ast_namespace,
                    type.type_name()
                )) {
                    add_symbols(*surface, false, k_bucket_own);
                }
            }
        }

        void add_symbols(const AST::Namespace &ns, bool types_only, char bucket)
        {
            for (const AST::Symbol *symbol : ns.symbols()) {
                if (AST::TypeDeclNode *type = _lookup.type_of_symbol(symbol)) {
                    add_type(*type, bucket);
                }
                else if (!types_only && symbol->type() == AST::SymbolType::t_constant
                    && symbol->node.has_type<AST::ConstDeclNode>()) {
                    add_constant(*symbol->node.get_ptr<AST::ConstDeclNode>(), bucket);
                }
            }
        }

        void add_namespace_children(const AST::Namespace &ns)
        {
            for (const AST::Namespace *child : ns.named_children()) {
                CompletionItem item;
                item.label = child->display_name();
                item.kind = CompletionItemKind::t_module;
                item.detail = "namespace " + child->full_name();
                add(std::move(item), k_bucket_module);
            }
        }

        // everything a namespace declares: namespaces, types, constants, and its free functions
        // (those live in the registry, so they are asked of it)
        void add_namespace(const AST::Namespace &ns, bool types_only)
        {
            add_namespace_children(ns);
            add_symbols(ns, types_only, k_bucket_module);

            AST::Bundle *bundle = _lookup.bundle();
            if (types_only || bundle == nullptr) {
                return;
            }

            for (AST::FunctionDeclNode *fn : bundle->collector.functions.get_all()) {
                if (fn != nullptr && fn->ast_namespace == &ns && !fn->is_member()) {
                    add_function(*fn, CompletionItemKind::t_function, bucket_of(fn->declared_in));
                }
            }
        }

        void add_static_path()
        {
            const bool variables_only = !_context.prefix.empty() && _context.prefix[0] == '$';

            if (AST::TypeDeclNode *type = _lookup.resolve_type_path(_context.path)) {
                add_statics(*type, variables_only);
            }

            if (variables_only) {
                return;
            }

            if (const AST::Namespace *ns = _lookup.resolve_namespace_path(_context.path)) {
                add_namespace(*ns, false);
            }
        }

        void add_shorthand()
        {
            AST::Bundle *bundle = _lookup.bundle();
            if (bundle == nullptr) {
                return;
            }

            AST::TypeDeclNode *destination = nullptr;
            if (!_context.destination_type.empty()) {
                destination = _lookup.resolve_type_text(_context.destination_type);
            }
            else if (!_context.destination_chain.empty()) {
                if (const auto type = _lookup.type_of_chain(_context.destination_chain)) {
                    destination = Compiler::Lsp::type_decl_of(*bundle, type.value());
                }
            }
            else if (_context.destination_is_return) {
                if (const AST::FunctionDeclNode *fn = _lookup.enclosing_function()) {
                    destination = Compiler::Lsp::type_decl_of(*bundle, fn->get_return_type());
                }
            }

            if (destination == nullptr) {
                return;
            }

            const AST::ComplexType &complex = destination->complex_type();
            for (const AST::ComplexType::EnumCase &entry : complex.enum_cases()) {
                CompletionItem item;
                item.label = entry.name;
                item.kind = CompletionItemKind::t_enum_member;
                item.label_description = destination->type_name();
                item.detail = "case " + entry.name;
                if (entry.has_payload() && _request.snippets) {
                    item.insert_text = entry.name + "($0)";
                    item.is_snippet = true;
                }
                add(std::move(item), k_bucket_own);
            }

            // a static that makes one: `.zero()`, `.from(...)`
            for (AST::FunctionDeclNode *fn : complex.template_or_self()->static_methods()) {
                if (complex_of(fn->get_return_type()) == &complex || complex_of(fn->get_return_type()) == complex.template_or_self()) {
                    add_function(*fn, CompletionItemKind::t_method, k_bucket_own);
                }
            }
        }

        void add_attributes()
        {
            const std::vector<std::string_view> names = _request.is_manifest
                ? AST::manifest_attribute_names()
                : AST::declaration_attribute_names();
            for (std::string_view name : names) {
                CompletionItem item;
                item.label = std::string(name);
                item.kind = CompletionItemKind::t_keyword;
                item.detail = "attribute";
                add(std::move(item), k_bucket_own);
            }

            // the conditional-compilation directives share the brackets
            for (const char *directive : { "if", "elif", "else", "end" }) {
                CompletionItem item;
                item.label = directive;
                item.kind = CompletionItemKind::t_keyword;
                item.detail = "conditional compilation";
                add(std::move(item), k_bucket_module);
            }
        }

        void add_keywords()
        {
            for (const std::string &keyword : keywords_for(_context.position)) {
                CompletionItem item;
                item.label = keyword;
                item.kind = CompletionItemKind::t_keyword;

                // at the start of a statement a keyword is the likeliest thing typed
                const bool leads = _context.position == NamePosition::t_statement
                    || _context.position == NamePosition::t_file_root
                    || _context.position == NamePosition::t_type_body
                    || _context.position == NamePosition::t_after_expression;
                add(std::move(item), leads ? k_bucket_own : k_bucket_keyword);
            }
        }

        void add_primitives()
        {
            for (const std::string &name : primitive_names()) {
                CompletionItem item;
                item.label = name;
                item.kind = CompletionItemKind::t_keyword;
                item.detail = "primitive type";
                add(std::move(item), k_bucket_module);
            }
        }

        void add_imports(bool types_only)
        {
            for (const AST::ImportBinding &binding : _request.file->imports) {
                if (binding.target_namespace == nullptr || binding.local_name.empty()) {
                    continue;
                }

                if (binding.kind == AST::ImportKind::t_namespace) {
                    CompletionItem item;
                    item.label = binding.local_name;
                    item.kind = CompletionItemKind::t_module;
                    item.detail = "use " + binding.target_namespace->full_name();
                    add(std::move(item), k_bucket_own);
                    continue;
                }

                if (AST::TypeDeclNode *type = _lookup.type_of_symbol(
                        _lookup.bundle()->collector.namespaces.find_symbol(
                            binding.target_name, *binding.target_namespace))) {
                    CompletionItem item;
                    item.label = binding.local_name;
                    item.kind = kind_of_type(*type);
                    item.detail = type->value_type().get_type_desciption();
                    add(std::move(item), k_bucket_own);
                    continue;
                }

                if (types_only) {
                    continue;
                }

                if (const AST::FunctionDeclNode *fn = _lookup.first_overload(binding.target_name, *binding.target_namespace)) {
                    if (is_completable(*fn)) {
                        CompletionItem item = callable_item(*fn, CompletionItemKind::t_function);
                        item.label = binding.local_name;
                        if (item.is_snippet) {
                            item.insert_text = binding.local_name + "($0)";
                        }
                        add(std::move(item), k_bucket_own);
                    }
                }
            }
        }

        void add_identifiers()
        {
            const NamePosition position = _context.position;
            add_keywords();

            if (position == NamePosition::t_after_expression) {
                return;
            }

            const bool types_only = position == NamePosition::t_type || position == NamePosition::t_type_body;
            if (types_only || position == NamePosition::t_statement || position == NamePosition::t_file_root) {
                add_primitives();
            }

            AST::Bundle *bundle = _lookup.bundle();
            if (bundle == nullptr) {
                return;
            }

            add_imports(types_only);

            // the cursor's namespace and every one enclosing it, nearest first. what a name written
            // here can reach without a `::`
            const AST::Namespace *here = _lookup.current_namespace();
            for (const AST::Namespace *ns = here; ns != nullptr; ns = ns->parent()) {
                add_symbols(*ns, types_only, ns == here ? k_bucket_module : k_bucket_other);
                add_namespace_children(*ns);
            }

            if (types_only) {
                return;
            }

            std::unordered_set<const AST::Namespace *> reachable;
            for (const AST::Namespace *ns = here; ns != nullptr; ns = ns->parent()) {
                reachable.insert(ns);
            }

            for (AST::FunctionDeclNode *fn : bundle->collector.functions.get_all()) {
                if (fn != nullptr && !fn->is_member() && reachable.count(fn->ast_namespace) > 0) {
                    add_function(*fn, CompletionItemKind::t_function, bucket_of(fn->declared_in));
                }
            }
        }
    };
};

Compiler::Lsp::CompletionAnswer Compiler::Lsp::completion(const CompletionRequest &request)
{
    Completer completer(request);
    return completer.run();
}
