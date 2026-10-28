#include "Compiler/DeclsJson.h"

#include "AST/TypeAliasDeclNode.h"
#include "AST/ASTAccess.h"
#include "AST/ASTAttributes.h"
#include "AST/ASTConstness.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTExternInterface.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"
#include "AST/ASTTypeParam.h"
#include "AST/ASTValueType.h"
#include "AST/ConstDeclNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"
#include "Token.h"
#include "eco.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

typedef nlohmann::ordered_json Json;

Json json_attribute_value(const AST::AttributeValue &value)
{
    Json out = Json::object();

    switch (value.kind) {
    case AST::AttributeValueKind::t_string:
        out["kind"] = "string";
        out["text"] = value.text;
        break;
    case AST::AttributeValueKind::t_int:
        out["kind"] = "int";
        out["integer"] = value.integer;
        break;
    case AST::AttributeValueKind::t_float:
        out["kind"] = "float";
        out["number"] = value.number;
        break;
    case AST::AttributeValueKind::t_bool:
        out["kind"] = "bool";
        out["boolean"] = value.boolean;
        break;
    case AST::AttributeValueKind::t_name:
        out["kind"] = "name";
        out["text"] = value.text;
        break;
    case AST::AttributeValueKind::t_list:
        out["kind"] = "list";
        out["items"] = Json::array();
        for (const AST::AttributeValue &item : value.items) {
            out["items"].push_back(json_attribute_value(item));
        }
        break;
    case AST::AttributeValueKind::t_record:
        out["kind"] = "record";
        out["fields"] = Json::array();
        for (const AST::AttributeField &field : value.fields) {
            out["fields"].push_back({
                { "key", field.key },
                { "value", json_attribute_value(field.value) },
            });
        }
        break;
    }

    if (value.has_tag()) {
        out["tag"] = value.tag();
    }

    return out;
}

const char *visibility_name(AST::Visibility visibility)
{
    switch (visibility) {
    case AST::Visibility::t_public:
        return "public";
    case AST::Visibility::t_module:
        return "internal";
    case AST::Visibility::t_file:
        return "private";
    case AST::Visibility::t_owner:
        return "private";
    }

    assert(false && "unhandled Visibility in visibility_name");
    return "internal";
}

// core types plus per-file path and constant-name tables, built once
struct Emit
{
    const AST::CoreTypes &core;

    // path relative to the nearest module.eco
    std::unordered_map<const AST::File *, std::string> paths;

    // namespaced name. ConstDeclNode does not store its namespace, so this is read back off the tree
    std::unordered_map<const AST::ConstDeclNode *, std::string> constant_names;

    // constants in a struct body, by owner, written order
    std::unordered_map<const AST::ComplexType *, std::vector<const AST::ConstDeclNode *>> owned_constants;

    std::string path_of(const AST::DeclarationOrigin &origin);
};

// **the path a reader can open**: relative to the nearest `module.eco`. walked by file name because the
// bundle does not carry the manifest path. the main module is loose files, so it stays a basename
std::string Emit::path_of(const AST::DeclarationOrigin &origin)
{
    if (origin.file == nullptr) {
        return "";
    }

    const auto found = paths.find(origin.file);
    if (found != paths.end()) {
        return found->second;
    }

    const std::filesystem::path &path = origin.file->get_path();
    std::string out = path.filename().generic_string();

    if (origin.module != nullptr && origin.module->name != ECO_MAIN_MODULE_NAME) {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);

        for (std::filesystem::path dir = absolute.parent_path(); !error && !dir.empty(); dir = dir.parent_path()) {
            if (std::filesystem::exists(dir / "module.eco", error)) {
                const std::filesystem::path relative = std::filesystem::relative(absolute, dir, error);
                if (!error && !relative.empty()) {
                    out = relative.generic_string();
                }
                break;
            }

            if (dir == dir.parent_path()) {
                break;
            }
        }
    }

    paths.emplace(origin.file, out);
    return out;
}

Json json_location(
    Emit &emit,
    const AST::DeclarationOrigin &origin,
    const std::optional<TokenReference> &token
)
{
    Json out = Json::object();

    // origin is missing on a free operator. the name token still knows the file, and the file its module
    AST::DeclarationOrigin where = origin;
    if (where.file == nullptr && token.has_value() && token->is_valid() && token->file() != nullptr) {
        where.file = token->file();
        if (where.module == nullptr) {
            where.module = token->file()->module;
        }
    }

    const std::string file = emit.path_of(where);
    out["file"] = file.empty() ? Json(nullptr) : Json(file);

    if (token.has_value()) {
        out["line"] = token->line();
        out["column"] = token->column();
    }
    else {
        out["line"] = 0;
        out["column"] = 0;
    }

    return out;
}

// source text [first, past_end). from the file, as spelled. null if minted, cross-file, or no content
Json source_text(const TokenCollection &tokens, size_t first, size_t past_end)
{
    if (first >= past_end || past_end >= tokens.size()) {
        return nullptr;
    }

    const TokenReference start = tokens[first];
    const TokenReference end = tokens[past_end];
    const AST::File *file = start.file();

    if (file == nullptr || end.file() != file || start.is_minted() || end.is_minted()
        || !file->content.has_value() || end.line() < start.line()) {
        return nullptr;
    }

    std::string text;

    for (uint32_t line = start.line(); line <= end.line(); line++) {
        std::string content = file->get_content_of_line(line);
        if (!content.empty() && content.back() == '\r') {
            content.pop_back();
        }

        // columns are 1-based byte offsets
        const size_t from = line == start.line() ? start.column() - 1 : 0;
        const size_t to = line == end.line() ? end.column() - 1 : content.size();

        if (from > to || to > content.size()) {
            return nullptr;
        }

        text += content.substr(from, to - from);

        if (line != end.line()) {
            text += '\n';
        }
    }

    const size_t left = text.find_first_not_of(" \t\n");
    if (left == std::string::npos) {
        return nullptr;
    }

    const size_t right = text.find_last_not_of(" \t\n");
    return text.substr(left, right - left + 1);
}

// **the initializer after `name`**: `=` is the next token, then until `,` `)` or `;` outside brackets.
// angle brackets are not counted, so `map<string, int32>()` ends at its comma
Json initializer_text(const TokenReference &name)
{
    const TokenCollection &tokens = name.get_collection_ref();
    const size_t assign = name.get_handle() + 1;

    if (assign >= tokens.size() || tokens[assign].type() != Token::Type::t_assign) {
        return nullptr;
    }

    int32_t depth = 0;
    size_t index = assign + 1;

    for (; index < tokens.size(); index++) {
        const Token::Type type = tokens[index].type();

        if (type == Token::Type::t_open_paren || type == Token::Type::t_open_bracket
            || type == Token::Type::t_open_brace) {
            depth++;
            continue;
        }

        if (type == Token::Type::t_close_paren || type == Token::Type::t_close_bracket
            || type == Token::Type::t_close_brace) {
            if (depth == 0) {
                break;
            }
            depth--;
            continue;
        }

        if (depth == 0 && (type == Token::Type::t_comma || type == Token::Type::t_semicolon)) {
            break;
        }
    }

    return source_text(tokens, assign + 1, index);
}

Json json_tools(const AST::AttributeList &attributes)
{
    Json out = Json::array();
    std::unordered_set<uint64_t> seen;

    for (AST::AttributeNode *attr : attributes.all()) {
        if (attr == nullptr) {
            continue;
        }

        const auto tool = AST::tool_attribute_name(attr->attribute_id.value());

        if (!tool.has_value()) {
            continue;
        }

        // keyed on position: one tool attribute written twice is two entries
        const uint64_t position =
            (static_cast<uint64_t>(attr->attribute_id.line()) << 32) | attr->attribute_id.column();
        if (!seen.insert(position).second) {
            continue;
        }

        Json entry = {
            { "namespace", tool->first },
            { "name", tool->second },
            { "value", attr->value.has_value()
                ? json_attribute_value(attr->value.value())
                : Json(nullptr) },
        };
        out.push_back(std::move(entry));
    }

    return out;
}

Json json_type(const AST::ValueType &type, const AST::CoreTypes &core);

Json json_type_shape(const AST::ValueType &type, const AST::CoreTypes &core)
{
    if (type.is_unknown()) {
        return { { "kind", "named" }, { "name", "unknown" } };
    }

    if (type.is_wrapped_optional()) {
        return { { "kind", "optional" }, { "of", json_type(type.optional_payload(), core) } };
    }

    if (type.is_nullable() && !type.is_pointer() && !type.is_weak() && !type.is_c_function()) {
        return {
            { "kind", "optional" },
            { "of", json_type(AST::ValueType::make_non_nullable(type), core) },
        };
    }

    if (type.is_pointer() || type.is_weak()) {
        const AST::ValueType &inner = type.is_weak() ? type.weak_target() : type.pointee();
        return {
            { "kind", "ptr" },
            { "to", json_type(inner, core) },
            { "borrow", type.is_pointer() && !type.is_nullable() },
            { "weak", type.is_weak() },
            { "const", type.is_const() },
            { "nullable", type.is_nullable() },
        };
    }

    if (type.has_signature()) {
        const AST::CallableSignature &sig = type.signature();
        Json params = Json::array();

        for (const AST::ValueType &param : sig.parameter_types) {
            params.push_back(json_type(param, core));
        }

        return {
            { "kind", "function" },
            { "return", json_type(sig.return_type, core) },
            { "params", std::move(params) },
            { "extern", type.is_c_function() },
        };
    }

    if (type.is_primitive()) {
        return {
            { "kind", "prim" },
            { "name", AST::get_primitive_name(type.get_primitive_type()) },
        };
    }

    if (type.is_type_param()) {
        return { { "kind", "named" }, { "name", type.get_type_param()->name } };
    }

    if (type.is_simd()) {
        return {
            { "kind", "named" },
            { "name", "simd" },
            { "args", Json::array({
                json_type(type.simd_element(), core),
                json_type(type.simd_length(), core),
            }) },
        };
    }

    if (type.is_inline_array()) {
        return {
            { "kind", "named" },
            { "name", "inline_array" },
            { "args", Json::array({
                json_type(type.array_element(), core),
                json_type(type.array_length(), core),
            }) },
        };
    }

    if (!type.has_complex_type()) {
        return { { "kind", "named" }, { "name", "unknown" } };
    }

    const AST::ComplexType *ct = type.get_complex_type();
    const AST::ComplexType *tmpl = ct->template_or_self();

    if (core.is_string(AST::ValueType::make_mutable(type))) {
        return { { "kind", "string" } };
    }

    if (tmpl == core.declared_template(AST::CoreTypeKind::t_array)) {
        Json of = ct->instantiation_args.empty()
            ? Json { { "kind", "named" }, { "name", "unknown" } }
            : json_type(ct->instantiation_args[0], core);
        return { { "kind", "array" }, { "of", std::move(of) } };
    }

    if (tmpl == core.declared_template(AST::CoreTypeKind::t_map)) {
        Json out = { { "kind", "map" } };

        if (ct->instantiation_args.size() >= 2) {
            out["key"] = json_type(ct->instantiation_args[0], core);
            out["value"] = json_type(ct->instantiation_args[1], core);
        }

        return out;
    }

    Json out = { { "kind", "named" }, { "name", tmpl->namespaced_name() } };

    if (!ct->instantiation_args.empty()) {
        Json args = Json::array();

        for (const AST::ValueType &arg : ct->instantiation_args) {
            args.push_back(json_type(arg, core));
        }

        out["args"] = std::move(args);
    }

    return out;
}

// shape plus pointee const, which is where `const string&` keeps it. omitted when unset
Json json_type(const AST::ValueType &type, const AST::CoreTypes &core)
{
    Json out = json_type_shape(type, core);

    if (type.is_const() && !type.is_pointer() && !type.is_weak() && out.is_object() && !out.contains("const")) {
        out["const"] = true;
    }

    return out;
}

Json json_generics(const std::vector<AST::TypeParamDecl *> &params, size_t skip)
{
    Json out = Json::array();

    for (size_t i = skip; i < params.size(); i++) {
        const AST::TypeParamDecl *param = params[i];

        if (param == nullptr) {
            continue;
        }

        out.push_back({
            { "name", param->name },
            { "constraint", param->constraint_spelling.empty()
                ? Json(nullptr) : Json(param->constraint_spelling) },
        });
    }

    return out;
}

Json json_params(const AST::FunctionDeclNode &fn, const AST::CoreTypes &core)
{
    Json out = Json::array();
    const size_t start = fn.implicit_arg_count();

    for (size_t i = start; i < fn.args.size(); i++) {
        const AST::VarDeclNode *arg = fn.args[i];

        if (arg == nullptr) {
            continue;
        }

        // the effect as written. access_effect_of infers `read` for every `const T&`, which is
        // noise here
        const char *mode = AST::access_effect_spelling(arg->access_effect);

        out.push_back({
            { "name", arg->name() },
            { "label", arg->label().empty() ? Json(nullptr) : Json(arg->label()) },
            { "type", json_type(
                arg->has_type() ? arg->type() : AST::ValueType::make_unknown(), core) },
            { "default", arg->init_expr != nullptr },
            { "default_value", arg->init_expr != nullptr ? initializer_text(arg->token_varname) : Json(nullptr) },
            { "mode", mode[0] == '\0' ? Json(nullptr) : Json(mode) },
            { "move", arg->takes_ownership },
        });
    }

    return out;
}

bool skip_function(const AST::FunctionDeclNode &fn)
{
    return fn.is_closure
        || fn.is_implicitly_generated
        || fn.is_instantiated()
        || fn.is_test()
        || fn.is_init();
}

const char *function_kind(const AST::FunctionDeclNode &fn)
{
    if (fn.is_operator()) {
        return "operator";
    }

    if (fn.is_constructor()) {
        return "constructor";
    }

    if (fn.is_destructor()) {
        return "destructor";
    }

    if (fn.is_static_method()) {
        return "static";
    }

    if (fn.has_receiver()) {
        return "method";
    }

    return "function";
}

Json json_function(Emit &emit, const AST::FunctionDeclNode &fn)
{
    const AST::CoreTypes &core = emit.core;
    Json loc = json_location(emit, fn.declared_in, fn.name_token);
    const std::string name = fn.is_operator() ? fn.operator_spelling() : fn.func_name();

    // qualify a member through its type. a constructor has no owner_type and is already the type
    // it builds
    const AST::ComplexType *owner = fn.owner_type != nullptr ? fn.owner_type->template_or_self() : nullptr;
    const std::string qualified = owner != nullptr
        ? owner->namespaced_name() + "::" + name
        : fn.namespaced_func_name();

    Json out = {
        { "kind", function_kind(fn) },
        { "name", name },
        { "qualified", qualified },
        { "owner", owner != nullptr ? Json(owner->namespaced_name()) : Json(nullptr) },
        { "const", AST::receiver_is_const(fn) },
        { "visibility", visibility_name(fn.visibility) },
        { "file", loc["file"] },
        { "line", loc["line"] },
        { "column", loc["column"] },
        { "doc", fn.doc_comment.empty() ? Json(nullptr) : Json(fn.doc_comment) },
        { "tools", json_tools(fn.attributes) },
        { "export", fn.export_name.has_value() ? Json(*fn.export_name) : Json(nullptr) },
        { "params", json_params(fn, core) },
        { "return", json_type(fn.get_return_type(), core) },
        { "generics", json_generics(fn.type_parameters, fn.inherited_type_param_count) },
    };

    // a requirement written with a body, which a conformer may leave out
    if (fn.default_decl != nullptr) {
        out["default"] = true;
    }

    return out;
}

Json json_field(const AST::VarDeclNode &field, const AST::CoreTypes &core)
{
    const Json default_value = initializer_text(field.token_varname);

    return {
        { "name", field.name() },
        { "visibility", visibility_name(field.visibility) },
        { "static", field.is_static() },
        { "type", json_type(
            field.has_type() ? field.type() : AST::ValueType::make_unknown(), core) },
        { "line", field.token_varname.line() },
        { "default", field.init_expr != nullptr || !default_value.is_null() },
        { "default_value", default_value },
        { "doc", field.doc_comment.empty() ? Json(nullptr) : Json(field.doc_comment) },
        { "tools", json_tools(field.attributes) },
    };
}

Json json_enum_case(
    const AST::ComplexType &type,
    const AST::ComplexType::EnumCase &entry,
    const AST::CoreTypes &core
)
{
    Json payload = Json::array();

    for (size_t i = 0; i < entry.payload_field_count; i++) {
        const size_t index = entry.first_payload_property + i;
        const std::string name = i < entry.payload_field_names.size()
            ? entry.payload_field_names[i]
            : type.get_property(index).name;
        payload.push_back({
            { "name", name },
            { "type", json_type(type.get_property_type(index), core) },
        });
    }

    // docblock is on the `case` token before the name. a case has no drained attributes, so no `tools`
    Json doc = nullptr;
    Json line = 0;
    const TokenSpan &span = entry.name_span;

    if (span.tokens != nullptr && span.start_index < span.tokens->size()) {
        line = (*span.tokens)[span.start_index].line();

        if (span.start_index > 0 && (*span.tokens)[span.start_index - 1].type() == Token::Type::t_case) {
            if (const DocComment *comment = span.tokens->doc_comment_at(span.start_index - 1)) {
                doc = comment->text;
            }
        }
    }

    Json out = {
        { "name", entry.name },
        { "discriminant", entry.discriminant },
        { "payload", std::move(payload) },
        { "line", std::move(line) },
        { "doc", std::move(doc) },
    };

    if (entry.backing_string.has_value()) {
        out["value"] = entry.backing_string.value();
    }

    return out;
}

bool skip_type(const AST::TypeDeclNode &decl)
{
    const AST::ComplexType &type = decl.complex_type();
    return type.is_optional || type.is_instantiated();
}

// **a constant.** no public type on the node, so type and value are source text between `const` and `;`
Json json_constant(Emit &emit, const AST::ConstDeclNode &decl, const std::string &qualified)
{
    const TokenCollection &tokens = decl.token_name.get_collection_ref();
    const size_t name = decl.token_name.get_handle();

    // walk back to `const`. no guess if that keyword is not found
    size_t keyword = name;
    for (size_t steps = 0; keyword > 0 && steps < 64; steps++) {
        keyword--;
        if (tokens[keyword].type() == Token::Type::t_const) {
            break;
        }
    }

    const bool found = tokens[keyword].type() == Token::Type::t_const;
    Json loc = json_location(emit, decl.declared_in, decl.token_name);

    return {
        { "kind", "const" },
        { "name", decl.name() },
        { "qualified", qualified },
        { "visibility", visibility_name(decl.visibility) },
        { "file", loc["file"] },
        { "line", loc["line"] },
        { "column", loc["column"] },
        { "doc", decl.doc_comment.empty() ? Json(nullptr) : Json(decl.doc_comment) },
        { "type", found ? source_text(tokens, keyword + 1, name) : Json(nullptr) },
        { "value", initializer_text(decl.token_name) },
    };
}

Json json_type_alias(Emit &emit, const AST::TypeAliasDeclNode &alias)
{
    Json loc = json_location(emit, alias.declared_in, alias.token_name);
    const std::string space = alias.ast_namespace != nullptr ? alias.ast_namespace->full_name() : "";
    const AST::TypeNode *target = alias.resolved_target();

    return {
        { "kind", "type alias" },
        { "name", alias.name() },
        { "qualified", space.empty() ? alias.name() : space + ECO_NAMESPACE_SEPARATOR + alias.name() },
        { "visibility", visibility_name(alias.visibility) },
        { "file", loc["file"] },
        { "line", loc["line"] },
        { "column", loc["column"] },
        { "doc", alias.doc_comment.empty() ? Json(nullptr) : Json(alias.doc_comment) },
        { "generics", json_generics(alias.type_parameters, 0) },
        { "target", target != nullptr ? json_type(target->type, emit.core) : Json(nullptr) },
    };
}

Json json_type_decl(Emit &emit, AST::TypeDeclNode &decl)
{
    const AST::CoreTypes &core = emit.core;
    const AST::ComplexType &type = decl.complex_type();
    const char *kind = type.is_opaque_kind() ? "opaque" : AST::type_kind_keyword(type.kind);
    Json loc = json_location(emit, type.declared_in, decl.name_token);
    Json conforms = Json::array();

    for (const AST::ValueType &conformance : type.conformances()) {
        conforms.push_back(json_type(conformance, core));
    }

    Json out = {
        { "kind", kind },
        { "name", decl.type_name() },
        { "qualified", decl.namespaced_type_name() },
        { "visibility", visibility_name(type.visibility) },
        { "file", loc["file"] },
        { "line", loc["line"] },
        { "column", loc["column"] },
        { "doc", decl.doc_comment.empty() ? Json(nullptr) : Json(decl.doc_comment) },
        { "tools", json_tools(decl.attributes) },
        { "generics", json_generics(decl.type_parameters(), 0) },
        { "conforms", std::move(conforms) },
    };

    const auto owned = emit.owned_constants.find(&type);
    if (owned != emit.owned_constants.end()) {
        Json constants = Json::array();

        for (const AST::ConstDeclNode *constant : owned->second) {
            constants.push_back(json_constant(emit, *constant, decl.namespaced_type_name() + "::" + constant->name()));
        }

        out["constants"] = std::move(constants);
    }

    if (type.is_enum_kind()) {
        out["backing"] = type.enum_backing.has_value()
            ? json_type(type.enum_backing.value(), core)
            : Json(nullptr);
        Json cases = Json::array();

        for (const AST::ComplexType::EnumCase &entry : type.enum_cases()) {
            cases.push_back(json_enum_case(type, entry, core));
        }

        out["cases"] = std::move(cases);
    }

    if (type.is_extern_interface_kind()) {
        Json requirements = Json::array();

        for (AST::FunctionDeclNode *member : AST::extern_interface_requirements(type)) {
            Json req = json_function(emit, *member);
            req["link"] = Json(member->requirement_link_name());
            requirements.push_back(std::move(req));
        }

        out["requirements"] = std::move(requirements);
        return out;
    }

    if (type.has_property_layout() && !type.is_enum_kind()) {
        Json fields = Json::array();

        for (AST::VarDeclNode *field : decl.properties()) {
            if (field != nullptr) {
                fields.push_back(json_field(*field, core));
            }
        }

        for (AST::VarDeclNode *field : type.static_properties()) {
            if (field != nullptr) {
                fields.push_back(json_field(*field, core));
            }
        }

        out["fields"] = std::move(fields);
    }

    auto emit_fn = [&](AST::FunctionDeclNode *member, Json &into) {
        if (member == nullptr || skip_function(*member)) {
            return;
        }

        into.push_back(json_function(emit, *member));
    };

    if (type.is_interface_kind()) {
        Json members = Json::array();

        for (AST::FunctionDeclNode *member : type.methods()) {
            emit_fn(member, members);
        }

        out["members"] = std::move(members);
    }
    else if (!type.is_opaque_kind()) {
        const bool is_enum = type.is_enum_kind();
        Json methods = Json::array();

        // skip_function already drops minted case constructors, `value()` and `from()`
        auto emit_method = [&](AST::FunctionDeclNode *member) {
            if (member == nullptr || skip_function(*member) || member->is_constructor()
                || member->is_destructor()) {
                return;
            }

            methods.push_back(json_function(emit, *member));
        };

        for (AST::FunctionDeclNode *member : type.methods()) {
            emit_method(member);
        }

        for (AST::FunctionDeclNode *member : type.static_methods()) {
            emit_method(member);
        }

        if (is_enum) {
            // payload fields live in `cases`. only statics remain
            Json fields = Json::array();

            for (AST::VarDeclNode *field : type.static_properties()) {
                if (field != nullptr) {
                    fields.push_back(json_field(*field, core));
                }
            }

            out["fields"] = std::move(fields);
            out["methods"] = std::move(methods);
            return out;
        }

        Json constructors = Json::array();

        for (AST::FunctionDeclNode *ctor : decl.constructors()) {
            emit_fn(ctor, constructors);
        }

        out["constructors"] = std::move(constructors);
        out["methods"] = std::move(methods);
        out["destructor"] = type.destructor() != nullptr;
    }

    return out;
}

// namespaced names for constants under `ns`. skip lexical scopes: a body-local constant is not surface
void collect_constant_names(
    const AST::Namespace &ns,
    std::unordered_map<const AST::ConstDeclNode *, std::string> &into
)
{
    const std::string prefix = ns.is_root() ? "" : ns.full_name() + "::";

    for (const AST::Symbol *symbol : ns.symbols()) {
        if (symbol == nullptr || symbol->type() != AST::SymbolType::t_constant) {
            continue;
        }

        if (const AST::ConstDeclNode *constant = symbol->node.get_ptr<AST::ConstDeclNode>()) {
            into.emplace(constant, prefix + symbol->name());
        }
    }

    for (const AST::Namespace *child : ns.named_children()) {
        if (child != nullptr) {
            collect_constant_names(*child, into);
        }
    }
}

}

std::string Compiler::decls_as_json(
    const AST::Bundle &bundle,
    const std::unordered_set<std::string> &user_modules,
    bool include_all
)
{
    Emit emit { bundle.collector.core_types, {}, {}, {} };

    if (const AST::Namespace *root = bundle.collector.namespaces.get(std::vector<std::string> {})) {
        collect_constant_names(*root, emit.constant_names);
    }

    Json modules = Json::array();

    for (const auto &module_ptr : bundle.modules) {
        AST::Module &module = *module_ptr;

        if (!include_all && user_modules.count(module.name) == 0) {
            continue;
        }

        // constants first so a struct's own land on its type. keyed on position: one node can be
        // reached twice
        std::vector<const AST::ConstDeclNode *> free_constants;
        std::unordered_set<uint64_t> seen;

        for (AST::ConstDeclNode *constant : module.nodes.of_type<AST::ConstDeclNode>()) {
            if (constant == nullptr) {
                continue;
            }

            const uint64_t position =
                (static_cast<uint64_t>(constant->token_name.line()) << 32) | constant->token_name.column();
            if (!seen.insert(position).second) {
                continue;
            }

            if (constant->owner != nullptr) {
                emit.owned_constants[constant->owner->template_or_self()].push_back(constant);
            }
            else {
                free_constants.push_back(constant);
            }
        }

        Json declarations = Json::array();

        for (AST::TypeDeclNode *decl : module.nodes.of_type<AST::TypeDeclNode>()) {
            if (decl == nullptr || skip_type(*decl)) {
                continue;
            }

            declarations.push_back(json_type_decl(emit, *decl));
        }

        for (AST::FunctionDeclNode *fn : module.nodes.of_type<AST::FunctionDeclNode>()) {
            if (fn == nullptr || skip_function(*fn) || fn->is_member() || fn->is_constructor()) {
                continue;
            }

            declarations.push_back(json_function(emit, *fn));
        }

        for (const AST::TypeAliasDeclNode *alias : module.nodes.of_type<AST::TypeAliasDeclNode>()) {
            if (alias != nullptr) {
                declarations.push_back(json_type_alias(emit, *alias));
            }
        }

        for (const AST::ConstDeclNode *constant : free_constants) {
            const auto named = emit.constant_names.find(constant);
            const std::string qualified = named != emit.constant_names.end() ? named->second : constant->name();
            declarations.push_back(json_constant(emit, *constant, qualified));
        }

        modules.push_back({
            { "name", module.name },
            { "declarations", std::move(declarations) },
        });
    }

    Json root = { { "modules", std::move(modules) } };
    return root.dump(2) + "\n";
}
