#include "Compiler/Lsp/LspRename.h"

#include "AST/ASTBundle.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTImport.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTSymbol.h"
#include "AST/ASTValueType.h"
#include "AST/ConstDeclNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "Compiler/Lsp/LspCompletionContext.h"
#include "Compiler/Lsp/LspLiveText.h"
#include "Compiler/Lsp/LspQuery.h"
#include "Compiler/Lsp/LspResolve.h"
#include "Compiler/Lsp/LspUri.h"
#include "Compiler/SettledPath.h"
#include "Token.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>
#include <unordered_map>

namespace
{
    typedef std::tuple<std::string, uint32_t, uint32_t> EditKey;

    // a refusal names at most this many places a rename could not follow, so the message stays a line
    constexpr size_t k_max_listed_misses = 3;

    bool is_identifier(const std::string &name)
    {
        if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
            return false;
        }

        return std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        });
    }

    bool is_primitive_name(const std::string &name)
    {
        for (int p = static_cast<int>(AST::ValueTypePrimitive::t_int8);
            p <= static_cast<int>(AST::ValueTypePrimitive::t_void); p++) {
            if (AST::get_primitive_name(static_cast<AST::ValueTypePrimitive>(p)) == name) {
                return true;
            }
        }

        return false;
    }

    // the text a span covers in the file the snapshot compiled
    std::string text_at(const AST::Span &span)
    {
        if (span.file == nullptr || span.start.line != span.end.line || span.end.column <= span.start.column) {
            return "";
        }

        const std::string line = span.file->get_content_of_line(span.start.line);
        if (span.start.column == 0 || span.start.column - 1 >= line.size()) {
            return "";
        }

        return line.substr(span.start.column - 1, span.end.column - span.start.column);
    }

    bool in_workspace(const std::filesystem::path &path, const std::filesystem::path &root)
    {
        if (root.empty()) {
            return true;
        }

        const std::filesystem::path relative = path.lexically_relative(root);
        return !relative.empty() && *relative.begin() != "..";
    }

    // the declaration under the cursor, and what renaming it would mean
    struct Target
    {
        AST::Node *node = nullptr;
        const TokenReference *name_token = nullptr;
        std::string old_name;
        bool sigil = false;

        AST::FunctionDeclNode *function = nullptr;
        AST::TypeDeclNode *type = nullptr;
        AST::ConstDeclNode *constant = nullptr;
        AST::VarDeclNode *variable = nullptr;

        // a variable that is a property (or static property) of this type
        AST::TypeDeclNode *property_owner = nullptr;

        // a variable that is a parameter of this function
        AST::FunctionDeclNode *parameter_of = nullptr;

        AST::Span range;
        std::string refusal;
    };

    AST::TypeDeclNode *owner_of_property(AST::Bundle &bundle, const AST::VarDeclNode *variable)
    {
        for (auto &module : bundle.modules) {
            for (AST::TypeDeclNode *type : module->nodes.of_type<AST::TypeDeclNode>()) {
                const auto &properties = type->properties();
                const auto &statics = type->complex_type().static_properties();
                if (std::find(properties.begin(), properties.end(), variable) != properties.end()
                    || std::find(statics.begin(), statics.end(), variable) != statics.end()) {
                    return type;
                }
            }
        }

        return nullptr;
    }

    AST::FunctionDeclNode *function_of_parameter(AST::Bundle &bundle, const AST::VarDeclNode *variable)
    {
        for (auto &module : bundle.modules) {
            for (AST::FunctionDeclNode *fn : module->nodes.of_type<AST::FunctionDeclNode>()) {
                if (!fn->is_instantiated() && std::find(fn->args.begin(), fn->args.end(), variable) != fn->args.end()) {
                    return fn;
                }
            }
        }

        return nullptr;
    }

    // this function's own declarations. a nested closure is a different function, so a rename
    // that would collide with a name it declared is its problem
    class FunctionLocals : public AST::RecursiveVisitor
    {
    public:

        std::vector<AST::VarDeclNode *> decls;

        explicit FunctionLocals(const AST::FunctionDeclNode *fn) :
            _fn(fn)
        {}

        void visitFunctionDecl(AST::FunctionDeclNode &node) override
        {
            if (&node != _fn) {
                return;
            }

            AST::RecursiveVisitor::visitFunctionDecl(node);
        }

        void visitVarDecl(AST::VarDeclNode &node) override
        {
            decls.push_back(&node);
            AST::RecursiveVisitor::visitVarDecl(node);
        }

    private:

        const AST::FunctionDeclNode *_fn = nullptr;
    };

    AST::FunctionDeclNode *function_of_local(AST::Bundle &bundle, const AST::VarDeclNode *variable)
    {
        for (auto &module : bundle.modules) {
            for (AST::FunctionDeclNode *fn : module->nodes.of_type<AST::FunctionDeclNode>()) {
                if (fn->is_instantiated()) {
                    continue;
                }

                FunctionLocals vars(fn);
                fn->accept(vars);
                if (std::find(vars.decls.begin(), vars.decls.end(), variable) != vars.decls.end()) {
                    return fn;
                }
            }
        }

        return nullptr;
    }

    const AST::Namespace *namespace_of_constant(
        const AST::Namespace &ns,
        const AST::ConstDeclNode &constant
    )
    {
        for (const AST::Symbol *symbol : ns.symbols()) {
            if (symbol->type() == AST::SymbolType::t_constant
                && symbol->node.has_type<AST::ConstDeclNode>()
                && symbol->node.get_ptr<AST::ConstDeclNode>() == &constant) {
                return &ns;
            }
        }

        for (const AST::Namespace *child : ns.named_children()) {
            if (const AST::Namespace *found = namespace_of_constant(*child, constant)) {
                return found;
            }
        }

        return nullptr;
    }

    // this method answers an interface requirement. renaming it alone would break the
    // conformance, and renaming the requirement too is a different, larger edit
    bool implements_requirement(const AST::FunctionDeclNode &fn)
    {
        if (fn.owner_type == nullptr) {
            return false;
        }

        for (const AST::ValueType &interface : fn.owner_type->template_or_self()->conformances()) {
            if (!interface.has_complex_type()) {
                continue;
            }

            for (AST::FunctionDeclNode *requirement : interface.get_complex_type()->template_or_self()->methods()) {
                if (requirement->func_name() == fn.func_name()) {
                    return true;
                }
            }
        }

        return false;
    }

    std::string function_refusal(const AST::FunctionDeclNode &fn)
    {
        if (fn.is_constructor() || fn.is_destructor() || fn.is_init()) {
            return "A constructor, destructor or init block is named by its type. Rename the type instead.";
        }

        if (fn.is_operator()) {
            return "An operator is named by its symbol and cannot be renamed.";
        }

        if (fn.is_test() || fn.is_closure || fn.is_implicitly_generated) {
            return "This function has no name of its own to rename.";
        }

        if (fn.is_interface_requirement()) {
            return "'" + fn.func_name() + "' is an interface requirement. Renaming it means renaming every implementation with it, which rename does not do.";
        }

        if (implements_requirement(fn)) {
            return "'" + fn.func_name() + "' satisfies an interface requirement. Renaming it alone would break the conformance.";
        }

        return "";
    }

    Target resolve_target(
        const Compiler::Lsp::Snapshot &snapshot,
        const AST::File &file,
        AST::Location location,
        const std::filesystem::path &root
    )
    {
        Target target;
        AST::Bundle &bundle = *snapshot.bundle;

        const Compiler::Lsp::PositionIndex::Entry *hit = snapshot.index.entry_at(&file, location.line, location.column);
        if (hit == nullptr || hit->node == nullptr) {
            target.refusal = "There is no name here to rename.";
            return target;
        }

        target.range.file = &file;
        target.range.start = AST::Location{ hit->line, hit->column };
        target.range.end = AST::Location{ hit->line, hit->column + (hit->width > 0 ? hit->width : 1) };

        std::unordered_map<AST::Node *, AST::Node *> cache;
        target.node = Compiler::Lsp::reference_target(hit->node, bundle, cache);
        target.name_token = target.node != nullptr ? Compiler::Lsp::name_token_of(target.node) : nullptr;
        if (target.name_token == nullptr || !target.name_token->is_valid() || target.name_token->file() == nullptr) {
            target.refusal = "This name does not resolve to a declaration that can be renamed.";
            return target;
        }

        const std::filesystem::path home = Compiler::canonical_or_absolute(target.name_token->file()->get_path());
        if (Compiler::Lsp::is_stdlib_file(*target.name_token->file())) {
            target.refusal = "This is declared in the standard library, which cannot be renamed from here.";
            return target;
        }

        if (!in_workspace(home, root)) {
            target.refusal = "This is declared outside this workspace (" + home.string() + "), so not every use of it can be found.";
            return target;
        }

        const AST::NodeReference ref = AST::make_ref(target.node);
        if (ref.has_type<AST::FunctionDeclNode>()) {
            target.function = ref.get_ptr<AST::FunctionDeclNode>();
            target.old_name = target.function->func_name();
            target.refusal = function_refusal(*target.function);
        }
        else if (ref.has_type<AST::TypeDeclNode>()) {
            target.type = ref.get_ptr<AST::TypeDeclNode>();
            target.old_name = target.type->type_name();
        }
        else if (ref.has_type<AST::ConstDeclNode>()) {
            target.constant = ref.get_ptr<AST::ConstDeclNode>();
            target.old_name = target.constant->name();
        }
        else if (ref.has_type<AST::VarDeclNode>()) {
            target.variable = ref.get_ptr<AST::VarDeclNode>();
            target.old_name = target.variable->name();
            target.sigil = true;
            if (target.variable->name_full() == "$this") {
                target.refusal = "'$this' is the receiver, not a variable, and cannot be renamed.";
            }
            target.property_owner = owner_of_property(bundle, target.variable);
            if (target.property_owner == nullptr) {
                target.parameter_of = function_of_parameter(bundle, target.variable);
            }
        }
        else {
            target.refusal = "Only a variable, a property, a function, a method, a type or a constant can be renamed.";
        }

        return target;
    }

    // `$name:` arguments inside one call's parentheses, the tokens a named argument was written with.
    // read off the tokens because the resolved call no longer has them: CallResolver rewrote it
    // positional, and its argument names went with that
    std::vector<TokenReference> named_arguments_in(const AST::FunctionCallExprNode &call, const std::string &spelling)
    {
        std::vector<TokenReference> out;
        const TokenReference &name = call.token_function_name;
        if (!name.is_valid() || name.is_minted()) {
            return out;
        }

        const TokenCollection &tokens = name.get_collection_ref();
        size_t index = name.get_handle() + 1;
        int depth = 0;
        for (; index < tokens.tokens.size(); index++) {
            const TokenReference token(tokens, index);
            if (token.file() != name.file()) {
                break;
            }

            if (token.type() == Token::Type::t_open_paren) {
                depth++;
                continue;
            }

            if (token.type() == Token::Type::t_close_paren) {
                if (--depth <= 0) {
                    break;
                }
                continue;
            }

            if (depth == 1 && token.type() == Token::Type::t_varname && token.value() == spelling
                && index + 1 < tokens.tokens.size()
                && TokenReference(tokens, index + 1).type() == Token::Type::t_colon) {
                out.push_back(token);
            }

            // before the parentheses open, anything but a generic argument list means this is
            // some other name
            if (depth == 0 && token.type() != Token::Type::t_open_angle && token.type() != Token::Type::t_close_angle
                && token.type() != Token::Type::t_identifier && token.type() != Token::Type::t_namespace_sep
                && token.type() != Token::Type::t_comma && token.type() != Token::Type::t_op_shr) {
                break;
            }
        }

        return out;
    }

    bool matches_type(const AST::ValueType &type, const AST::ComplexType *wanted)
    {
        const AST::ValueType target = AST::target_type_of(type);
        return target.has_complex_type() && target.get_complex_type()->template_or_self() == wanted;
    }

    // a constructor is a free function under the type's name with no owner_type. what says which
    // type it constructs is what it returns
    bool is_construction_of(const AST::FunctionCallExprNode &call, const AST::TypeDeclNode &type)
    {
        const AST::ComplexType *wanted = &type.complex_type();
        if (call.decl != nullptr && call.decl->is_constructor() && matches_type(call.decl->get_return_type(), wanted)) {
            return true;
        }

        return matches_type(call.constructed_type, wanted);
    }

    // the regions the compile never saw: `test` blocks (the LSP compiles without tests) and every
    // `#[if:]` arm (an inactive one is filtered out as tokens). a name inside one is a use rename
    // cannot see, unless the edits already cover it because the arm was the active one
    std::vector<AST::Location> unseen_mentions(const AST::File &file, const std::string &bare, const std::string &sigiled)
    {
        std::vector<AST::Location> out;
        if (!file.content.has_value()) {
            return out;
        }

        const std::string &text = file.content.value();
        bool in_code = true;
        const std::vector<Compiler::Lsp::LexToken> tokens = Compiler::Lsp::lex_echo(text, text.size(), in_code);
        const Compiler::Lsp::LiveText lines(text);

        int test_depth = 0;
        int cond_depth = 0;
        int depth = 0;

        for (size_t i = 0; i < tokens.size(); i++) {
            const Compiler::Lsp::LexToken &token = tokens[i];
            const Compiler::Lsp::LexToken *next = i + 1 < tokens.size() ? &tokens[i + 1] : nullptr;
            const Compiler::Lsp::LexToken *after = i + 2 < tokens.size() ? &tokens[i + 2] : nullptr;

            if (token.kind == Compiler::Lsp::LexKind::t_punct && token.text == "#[" && next != nullptr) {
                if (next->text == "if") {
                    cond_depth++;
                }
                else if (next->text == "end") {
                    cond_depth = std::max(0, cond_depth - 1);
                }
            }

            if (token.kind == Compiler::Lsp::LexKind::t_identifier && token.text == "test" && test_depth == 0
                && next != nullptr && next->kind == Compiler::Lsp::LexKind::t_identifier
                && after != nullptr && after->text == "{") {
                test_depth = depth + 1;
            }

            if (token.kind == Compiler::Lsp::LexKind::t_punct && token.text == "{") {
                depth++;
            }
            else if (token.kind == Compiler::Lsp::LexKind::t_punct && token.text == "}") {
                if (test_depth != 0 && depth == test_depth) {
                    test_depth = 0;
                }
                depth = std::max(0, depth - 1);
            }

            const bool unseen = cond_depth > 0 || (test_depth != 0 && depth >= test_depth);
            if (unseen && (token.text == bare || token.text == sigiled)
                && token.kind != Compiler::Lsp::LexKind::t_string) {
                out.push_back(lines.location_of(token.offset));
            }
        }

        return out;
    }
};

Compiler::Lsp::PrepareRenameAnswer Compiler::Lsp::prepare_rename(
    const Snapshot &snapshot,
    const AST::File &file,
    AST::Location location,
    const std::filesystem::path &workspace_root
)
{
    PrepareRenameAnswer answer;
    const Target target = resolve_target(snapshot, file, location, workspace_root);
    answer.refusal = target.refusal;
    answer.range = target.range;
    answer.placeholder = text_at(target.range);
    return answer;
}

Compiler::Lsp::RenameAnswer Compiler::Lsp::rename(
    const Snapshot &snapshot,
    const AST::File &file,
    AST::Location location,
    const std::string &new_name,
    const std::filesystem::path &workspace_root
)
{
    RenameAnswer answer;
    const Target target = resolve_target(snapshot, file, location, workspace_root);
    if (!target.refusal.empty()) {
        answer.refusal = target.refusal;
        return answer;
    }

    AST::Bundle &bundle = *snapshot.bundle;

    // the name: `$` optional on a variable, refused anywhere else
    std::string bare = new_name;
    if (!bare.empty() && bare[0] == '$') {
        if (!target.sigil) {
            answer.refusal = "Only a variable or a property is spelled with '$'.";
            return answer;
        }
        bare = bare.substr(1);
    }

    if (!is_identifier(bare)) {
        answer.refusal = "'" + new_name + "' is not a name: it has to start with a letter or '_' and go on with letters, digits or '_'.";
        return answer;
    }

    if (token_is_reserved_word(bare) || bare == "this") {
        answer.refusal = "'" + bare + "' is a reserved word.";
        return answer;
    }

    if (target.type != nullptr && is_primitive_name(bare)) {
        answer.refusal = "'" + bare + "' is a primitive type's name.";
        return answer;
    }

    if (bare == target.old_name) {
        return answer;
    }

    // what is already called that, in the scope the new name would land in
    const std::string sigiled = "$" + bare;
    if (target.function != nullptr) {
        const AST::FunctionDeclNode &fn = *target.function;
        bool taken = false;
        if (fn.owner_type != nullptr) {
            for (const auto *members : { &fn.owner_type->methods(), &fn.owner_type->static_methods() }) {
                for (AST::FunctionDeclNode *other : *members) {
                    taken = taken || other->func_name() == bare;
                }
            }
        }
        else if (fn.ast_namespace != nullptr) {
            for (AST::FunctionDeclNode *other : bundle.collector.functions.overloads(bare, *fn.ast_namespace)) {
                taken = taken || other->ast_namespace == fn.ast_namespace;
            }
        }
        if (taken) {
            answer.refusal = "'" + bare + "' is already declared there.";
            return answer;
        }
    }
    else if (target.type != nullptr || target.constant != nullptr) {
        const AST::Namespace *ns = nullptr;
        if (target.type != nullptr) {
            ns = target.type->ast_namespace;
        }
        else {
            ns = namespace_of_constant(bundle.collector.namespaces.root(), *target.constant);
        }
        if (ns == nullptr) {
            ns = &bundle.collector.namespaces.root();
        }
        if (bundle.collector.namespaces.find_symbol(bare, *ns) != nullptr) {
            answer.refusal = "'" + bare + "' is already declared in that namespace.";
            return answer;
        }
    }
    else if (target.property_owner != nullptr) {
        const AST::ComplexType &owner = target.property_owner->complex_type();
        if (owner.has_property(bare) || owner.find_static_property(sigiled).has_value() || owner.find_static_property(bare).has_value()) {
            answer.refusal = "'" + target.property_owner->type_name() + "' already has a property named '" + bare + "'.";
            return answer;
        }
    }
    else if (target.variable != nullptr) {
        AST::FunctionDeclNode *fn = target.parameter_of != nullptr
            ? target.parameter_of
            : function_of_local(bundle, target.variable);
        if (fn != nullptr) {
            FunctionLocals vars(fn);
            fn->accept(vars);
            for (AST::VarDeclNode *other : vars.decls) {
                if (other != target.variable && other->name() == bare) {
                    answer.refusal = "'" + sigiled + "' is already declared in this function.";
                    return answer;
                }
            }
        }
    }

    std::set<EditKey> seen;
    auto push = [&](const AST::File &in, const AST::Span &span, const std::string &text) {
        const EditKey key{ in.get_path().string(), span.start.line, span.start.column };
        if (!seen.insert(key).second) {
            return;
        }
        answer.edits.push_back(RenameEdit{ in.get_path(), span, text });
    };

    // every use the index resolves to the target, as long as it is spelled with the target's own
    // name. an alias (`use a::f as g`, then `g()`) is the alias's spelling and stays put
    const std::string old_sigiled = "$" + target.old_name;
    for (const DefinitionAnswer &hit : references(snapshot, file, location, true)) {
        if (hit.range.file == nullptr) {
            continue;
        }

        const std::string written = text_at(hit.range);
        if (written == target.old_name) {
            push(*hit.range.file, hit.range, bare);
        }
        else if (written == old_sigiled) {
            push(*hit.range.file, hit.range, sigiled);
        }
    }

    for (const AST::File *indexed : snapshot.index.files()) {
        if (indexed == nullptr || is_stdlib_file(*indexed)) {
            continue;
        }

        // the uses the index answers as something else: `Point(1, 2)` resolves to the constructor,
        // `f($a: 1)` and `Point($x: 1)` to the call. both are still the target's name as written
        for (const PositionIndex::CallSite &site : snapshot.index.calls_of(indexed)) {
            const AST::FunctionCallExprNode *call = site.call;
            if (call == nullptr) {
                continue;
            }

            if (target.type != nullptr && is_construction_of(*call, *target.type)
                && call->token_function_name.value() == target.old_name) {
                push(*indexed, AST::span_of(call->token_function_name), bare);
            }

            const bool named_parameter = target.parameter_of != nullptr && call->decl != nullptr
                && canonical_target(call->decl) == target.parameter_of;
            const bool named_field = target.property_owner != nullptr && is_construction_of(*call, *target.property_owner);
            if (named_parameter || named_field) {
                for (const TokenReference &token : named_arguments_in(*call, old_sigiled)) {
                    push(*indexed, AST::span_of(token), sigiled);
                }
            }
        }

        // `use geo::area;` names the function by its last segment
        const AST::Namespace *home = target.function != nullptr ? target.function->ast_namespace
            : target.type != nullptr ? target.type->ast_namespace
            : nullptr;
        if (home == nullptr) {
            continue;
        }

        for (const AST::ImportBinding &binding : indexed->imports) {
            if (binding.kind != AST::ImportKind::t_item || binding.target_name != target.old_name
                || binding.target_namespace != home || !binding.span.has_value()) {
                continue;
            }

            const TokenSlice &slice = binding.span.value();
            for (size_t i = 0; i <= slice.size(); i++) {
                const TokenReference token = slice[i];
                if (token.type() == Token::Type::t_identifier && token.value() == target.old_name
                    && !(binding.local_token.has_value() && binding.local_token->get_handle() == token.get_handle()
                        && binding.local_name != target.old_name)) {
                    push(*indexed, AST::span_of(token), bare);
                }
            }
        }
    }

    // a name reachable from outside its function can appear where the compile did not look
    const bool is_local = target.variable != nullptr && target.property_owner == nullptr;
    if (!is_local) {
        std::vector<std::string> misses;
        for (const AST::File *indexed : snapshot.index.files()) {
            if (indexed == nullptr || is_stdlib_file(*indexed)) {
                continue;
            }

            for (const AST::Location &mention : unseen_mentions(*indexed, target.old_name, old_sigiled)) {
                if (seen.count(EditKey{ indexed->get_path().string(), mention.line, mention.column }) == 0) {
                    misses.push_back(indexed->get_path().filename().string() + ":" + std::to_string(mention.line));
                }
            }
        }

        if (!misses.empty()) {
            std::string listed;
            for (size_t i = 0; i < misses.size() && i < k_max_listed_misses; i++) {
                listed += (i > 0 ? ", " : "") + misses[i];
            }
            if (misses.size() > k_max_listed_misses) {
                listed += " and " + std::to_string(misses.size() - k_max_listed_misses) + " more";
            }

            answer.edits.clear();
            answer.refusal = "'" + target.old_name + "' is also written inside a test block or an #[if:] region, which the editor's compile does not see (" + listed + "). Rename it there by hand, or not at all.";
        }
    }

    return answer;
}
