#include "Compiler/DeclsJson.h"

#include "AST/ASTAttributes.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTTypeParam.h"
#include "AST/ASTValueType.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <unordered_set>

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

std::string file_basename(const AST::DeclarationOrigin &origin)
{
    if (origin.file == nullptr) {
        return "";
    }

    return origin.file->get_path().filename().generic_string();
}

Json json_location(
    const AST::DeclarationOrigin &origin,
    const std::optional<TokenReference> &token
)
{
    Json out = Json::object();
    const std::string file = file_basename(origin);
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

Json json_tools(const AST::AttributeList &attributes)
{
    Json out = Json::array();
    std::unordered_set<std::string> seen;

    for (AST::AttributeNode *attr : attributes.all()) {
        if (attr == nullptr) {
            continue;
        }

        const auto tool = AST::tool_attribute_name(attr->attribute_id.value());

        if (!tool.has_value()) {
            continue;
        }

        // each parse pass drains a fresh AttributeNode over the same tokens
        if (!seen.insert(attr->attribute_id.value()).second) {
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

Json json_type(const AST::ValueType &type, const AST::CoreTypes &core)
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

        out.push_back({
            { "name", arg->name() },
            { "label", arg->label().empty() ? Json(nullptr) : Json(arg->label()) },
            { "type", json_type(
                arg->has_type() ? arg->type() : AST::ValueType::make_unknown(), core) },
            { "default", arg->init_expr != nullptr },
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

Json json_function(const AST::FunctionDeclNode &fn, const AST::CoreTypes &core)
{
    Json loc = json_location(fn.declared_in, fn.name_token);
    Json out = {
        { "kind", function_kind(fn) },
        { "name", fn.is_operator() ? fn.operator_spelling() : fn.func_name() },
        { "qualified", fn.namespaced_func_name() },
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
    return out;
}

Json json_field(const AST::VarDeclNode &field, const AST::CoreTypes &core)
{
    return {
        { "name", field.name() },
        { "visibility", visibility_name(field.visibility) },
        { "static", field.is_static() },
        { "type", json_type(
            field.has_type() ? field.type() : AST::ValueType::make_unknown(), core) },
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

    Json out = {
        { "name", entry.name },
        { "discriminant", entry.discriminant },
        { "payload", std::move(payload) },
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

Json json_type_decl(AST::TypeDeclNode &decl, const AST::CoreTypes &core)
{
    const AST::ComplexType &type = decl.complex_type();
    const char *kind = type.is_opaque_kind() ? "opaque" : AST::type_kind_keyword(type.kind);
    Json loc = json_location(type.declared_in, decl.name_token);
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
    };

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

        for (AST::FunctionDeclNode *member : type.methods()) {
            if (member == nullptr) {
                continue;
            }

            Json req = json_function(*member, core);
            req["link"] = member->abi_link_name.has_value()
                ? Json(*member->abi_link_name)
                : Json(member->func_name());
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

        into.push_back(json_function(*member, core));
    };

    if (type.is_interface_kind()) {
        Json members = Json::array();

        for (AST::FunctionDeclNode *member : type.methods()) {
            emit_fn(member, members);
        }

        out["members"] = std::move(members);
    }
    else if (!type.is_enum_kind() && !type.is_opaque_kind()) {
        Json constructors = Json::array();

        for (AST::FunctionDeclNode *ctor : decl.constructors()) {
            emit_fn(ctor, constructors);
        }

        Json methods = Json::array();

        auto emit_method = [&](AST::FunctionDeclNode *member) {
            if (member == nullptr || skip_function(*member) || member->is_constructor()
                || member->is_destructor()) {
                return;
            }

            methods.push_back(json_function(*member, core));
        };

        for (AST::FunctionDeclNode *member : type.methods()) {
            emit_method(member);
        }

        for (AST::FunctionDeclNode *member : type.static_methods()) {
            emit_method(member);
        }

        out["constructors"] = std::move(constructors);
        out["methods"] = std::move(methods);
        out["destructor"] = type.destructor() != nullptr;
    }

    return out;
}

}

std::string Compiler::decls_as_json(
    const AST::Bundle &bundle,
    const std::unordered_set<std::string> &user_modules,
    bool include_all
)
{
    Json modules = Json::array();

    for (const auto &module_ptr : bundle.modules) {
        AST::Module &module = *module_ptr;

        if (!include_all && user_modules.count(module.name) == 0) {
            continue;
        }

        Json declarations = Json::array();

        for (AST::TypeDeclNode *decl : module.nodes.of_type<AST::TypeDeclNode>()) {
            if (decl == nullptr || skip_type(*decl)) {
                continue;
            }

            declarations.push_back(json_type_decl(*decl, bundle.collector.core_types));
        }

        for (AST::FunctionDeclNode *fn : module.nodes.of_type<AST::FunctionDeclNode>()) {
            if (fn == nullptr || skip_function(*fn) || fn->is_member() || fn->is_constructor()) {
                continue;
            }

            declarations.push_back(json_function(*fn, bundle.collector.core_types));
        }

        modules.push_back({
            { "name", module.name },
            { "declarations", std::move(declarations) },
        });
    }

    Json root = { { "modules", std::move(modules) } };
    return root.dump(2) + "\n";
}
