#include "AST/ASTCoreTypes.h"

#include "AST/TypeDeclNode.h"

#include <fmt/core.h>

namespace
{
    // the one table. a name is the surface spelling written in `#[core: ...]`, deliberately not the
    // type's own name - the two are free to differ, and a reader of the attribute should not have to
    // guess which of them the compiler keys on
    const std::unordered_map<std::string, AST::CoreTypeKind> &core_type_table()
    {
        static const std::unordered_map<std::string, AST::CoreTypeKind> table = {
            { "string", AST::CoreTypeKind::t_string },
            { "string_view", AST::CoreTypeKind::t_string_view },
            { "array", AST::CoreTypeKind::t_array },
            { "map", AST::CoreTypeKind::t_map },
            { "buffer", AST::CoreTypeKind::t_buffer },
            { "iterator", AST::CoreTypeKind::t_iterator },
            { "iterable", AST::CoreTypeKind::t_iterable },
            { "const_iterable", AST::CoreTypeKind::t_const_iterable },
            { "keyed", AST::CoreTypeKind::t_keyed },
            { "unwrappable", AST::CoreTypeKind::t_unwrappable },
            { "failable", AST::CoreTypeKind::t_failable },
            { "variadic_args", AST::CoreTypeKind::t_variadic_args },
            { "crash_info", AST::CoreTypeKind::t_crash_info },
            { "type_id", AST::CoreTypeKind::t_type_id },
            { "erased", AST::CoreTypeKind::t_erased },
        };

        return table;
    }
}

std::optional<AST::CoreTypeKind> AST::core_type_kind_for(const std::string &name)
{
    auto it = core_type_table().find(name);

    return it == core_type_table().end() ? std::nullopt : std::optional<AST::CoreTypeKind>(it->second);
}

void AST::CoreTypes::bind(AST::CoreTypeKind kind, AST::TypeDeclNode *decl)
{
    _bound[kind] = decl;
}

AST::TypeDeclNode *AST::CoreTypes::declaration(AST::CoreTypeKind kind) const
{
    auto it = _bound.find(kind);

    return it == _bound.end() ? nullptr : it->second;
}

AST::ValueType AST::CoreTypes::type(AST::CoreTypeKind kind) const
{
    AST::TypeDeclNode *decl = declaration(kind);

    return decl == nullptr ? AST::ValueType::make_unknown() : decl->value_type();
}

AST::ComplexType *AST::CoreTypes::declared_template(AST::CoreTypeKind kind) const
{
    AST::TypeDeclNode *decl = declaration(kind);

    return decl == nullptr ? nullptr : &decl->complex_type();
}

std::string AST::CoreTypes::spelling(AST::CoreTypeKind kind) const
{
    if (AST::TypeDeclNode *decl = declaration(kind)) {
        return decl->namespaced_type_name();
    }

    // the reverse of the one table. a linear scan over it, deliberately not a second map to keep
    // drifting out of sync with the first
    for (const auto &[tag, tagged_kind] : core_type_table()) {
        if (tagged_kind == kind) {
            return fmt::format("<core \"{}\">", tag);
        }
    }

    return "<core>";
}

const AST::ComplexType::Property *AST::CoreTypes::property(
    AST::CoreTypeKind kind,
    const char *name
) const
{
    AST::TypeDeclNode *decl = declaration(kind);

    if (decl == nullptr) {
        return nullptr;
    }

    return decl->complex_type().find_property(name);
}

std::optional<AST::CoreStringLayout> AST::resolve_core_string_layout(const AST::CoreTypes &types, std::string &out_error)
{
    AST::TypeDeclNode *string_decl = types.declaration(AST::CoreTypeKind::t_string);
    AST::TypeDeclNode *view_decl = types.declaration(AST::CoreTypeKind::t_string_view);

    if (string_decl == nullptr || view_decl == nullptr) {
        out_error = "no type is declared with #[core: string] and #[core: string_view]";
        return std::nullopt;
    }

    // named locally so the four lookups below read as one contract rather than four string literals
    // scattered through it
    auto require = [&out_error](const AST::ComplexType &ct, const char *property, size_t &out_index) {
        const AST::ComplexType::Property *found = ct.find_property(property);

        if (found == nullptr) {
            out_error = fmt::format("'{}' has no property '${}'", ct.namespaced_name(), property);
            return false;
        }

        out_index = found->index;
        return true;
    };

    AST::CoreStringLayout layout {};

    if (!require(string_decl->complex_type(), "window", layout.window_index)
        || !require(string_decl->complex_type(), "owner", layout.owner_index)
        || !require(view_decl->complex_type(), "bytes", layout.bytes_index)
        || !require(view_decl->complex_type(), "size", layout.size_index)) {
        return std::nullopt;
    }

    // the window has to *be* the bound view type, or the constant codegen builds would be the right
    // bytes in the wrong shape - which LLVM would accept as a type mismatch far from the cause
    if (string_decl->complex_type().get_property_type(layout.window_index)
        != types.string_view_type()) {
        out_error = fmt::format("'{}'s $window is not the #[core: string_view] type",
            string_decl->complex_type().namespaced_name());
        return std::nullopt;
    }

    // the owner carries the reference count, so it has to be a class - that is what makes a literal's
    // null owner free and a copy of a string one retain
    if (!string_decl->complex_type().get_property_type(layout.owner_index).is_class()) {
        out_error = fmt::format("'{}'s $owner must be a class - it is what holds the reference count",
            string_decl->complex_type().namespaced_name());
        return std::nullopt;
    }

    return layout;
}

std::optional<AST::CoreCrashInfoLayout> AST::resolve_core_crash_info_layout(
    const AST::CoreTypes &types,
    std::string &out_error
)
{
    AST::TypeDeclNode *info_decl = types.declaration(AST::CoreTypeKind::t_crash_info);

    if (info_decl == nullptr) {
        out_error = "no type is declared with #[core: crash_info]";
        return std::nullopt;
    }

    if (!types.has(AST::CoreTypeKind::t_string_view)) {
        out_error = "#[core: crash_info] needs #[core: string_view] for its text fields";
        return std::nullopt;
    }

    auto require = [&out_error](const AST::ComplexType &ct, const char *property, size_t &out_index) {
        const AST::ComplexType::Property *found = ct.find_property(property);

        if (found == nullptr) {
            out_error = fmt::format("'{}' has no property '${}'", ct.namespaced_name(), property);
            return false;
        }

        out_index = found->index;
        return true;
    };

    AST::CoreCrashInfoLayout layout {};
    const AST::ComplexType &info = info_decl->complex_type();

    if (!require(info, "headline", layout.headline_index)
        || !require(info, "message", layout.message_index)
        || !require(info, "file", layout.file_index)
        || !require(info, "line", layout.line_index)) {
        return std::nullopt;
    }

    if (info.property_count() != 4) {
        out_error = fmt::format(
            "'{}' must have exactly the four crash fields ($headline, $message, $file, $line)",
            info.namespaced_name());
        return std::nullopt;
    }

    AST::TypeDeclNode *view_decl = types.declaration(AST::CoreTypeKind::t_string_view);

    if (!require(view_decl->complex_type(), "bytes", layout.view_bytes_index)
        || !require(view_decl->complex_type(), "size", layout.view_size_index)) {
        return std::nullopt;
    }

    const AST::ValueType view = types.string_view_type();
    auto require_view = [&out_error, &info, &view](size_t index, const char *name) {
        if (info.get_property_type(index) != view) {
            out_error = fmt::format("'{}'s ${} is not the #[core: string_view] type",
                info.namespaced_name(), name);
            return false;
        }

        return true;
    };

    if (!require_view(layout.headline_index, "headline")
        || !require_view(layout.message_index, "message")
        || !require_view(layout.file_index, "file")) {
        return std::nullopt;
    }

    const AST::ValueType line = info.get_property_type(layout.line_index);

    if (!line.is_primitive() || line.get_primitive_type() != AST::ValueTypePrimitive::t_int32) {
        out_error = fmt::format("'{}'s $line must be 'int32'", info.namespaced_name());
        return std::nullopt;
    }

    return layout;
}
