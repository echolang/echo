#ifndef ASTCORETYPES_H
#define ASTCORETYPES_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace AST
{
    class TypeDeclNode;

    // the stdlib types the *compiler itself* has to be able to name, bound by `#[core: "..."]` on the
    // declaration.
    //
    // the compiler deliberately does not hardcode the name `string`, for the reason a builtin's surface
    // lives in the stdlib (ASTBuiltin.h): the name, the namespace, the documentation and the
    // "unknown type" diagnostic should all come from Echo source. This is the indirection that buys it.
    //
    // **every reader must cope with a kind being unbound.** a program compiled without the stdlib has
    // no `string`, and the compiler has to keep working on it - which is also what keeps the string
    // types themselves bootstrappable, since `stdlib/core/string.eco` is parsed by the same compiler
    enum class CoreTypeKind
    {
        // `struct string` - the owning, copy-on-write UTF-8 string. what a literal is
        t_string,

        // `string::view` - a borrowed window over UTF-8 bytes, owning nothing
        t_string_view,
    };

    // resolves a core-type name to its kind, or nullopt when the name is not one. the single place that
    // knows the set, so the parser can reject an unknown name where the attribute is written rather
    // than leaving a silently unbound slot to fail much later
    std::optional<CoreTypeKind> core_type_kind_for(const std::string &name);

    class CoreTypes
    {
    public:
        // binds a kind to the declaration that carried the attribute. the parse passes reach the same
        // declaration more than once, so re-binding the identical node is normal and silent; binding a
        // *different* one is a second `#[core: "string"]` in the program, which the caller reports
        void bind(CoreTypeKind kind, TypeDeclNode *decl);

        TypeDeclNode *declaration(CoreTypeKind kind) const;

        bool has(CoreTypeKind kind) const {
            return declaration(kind) != nullptr;
        }

        // the bound type, or an **unknown** ValueType when nothing declared this kind. answering
        // unknown rather than asserting is the whole contract: see the class comment
        ValueType type(CoreTypeKind kind) const;

        ValueType string_type() const {
            return type(CoreTypeKind::t_string);
        }

        ValueType string_view_type() const {
            return type(CoreTypeKind::t_string_view);
        }

        // **is this one of the two string types?** asked by `echo` and by `.`, so it is one predicate
        // rather than a comparison spelled at each site. const is dropped, because a `const string` is
        // still a string to something only reading it
        bool is_string(const ValueType &candidate) const {
            return has(CoreTypeKind::t_string)
                && ValueType::make_mutable(candidate) == string_type();
        }

        bool is_string_view(const ValueType &candidate) const {
            return has(CoreTypeKind::t_string_view)
                && ValueType::make_mutable(candidate) == string_view_type();
        }

        // either one. what a reader that only wants to *look at* bytes accepts, since the owning type
        // holds the borrowed one as a property and can always hand it over
        bool is_string_like(const ValueType &candidate) const {
            return is_string(candidate) || is_string_view(candidate);
        }

    private:
        std::unordered_map<CoreTypeKind, TypeDeclNode *> _bound;
    };

    // where the compiler finds the fields it must fill in when it builds a `string` constant for a
    // literal. this is the one place the compiler knows anything about the *shape* of the stdlib's
    // string, and it is deliberately resolved **by property name**: the stdlib is free to reorder its
    // fields, and only the four names below are the contract.
    //
    // the alternative was for codegen to assume "property 0 is the window, property 1 is the owner",
    // which is the same coupling with nothing to check it and no diagnostic when it drifts
    struct CoreStringLayout
    {
        // on `string`
        size_t window_index;
        size_t owner_index;

        // on `string::view`
        size_t bytes_index;
        size_t size_index;
    };

    // resolves the layout above, or answers nullopt and fills `out_error` with a message naming exactly
    // what the bound declarations are missing. absence of a binding is *not* an error here - a caller
    // asks CoreTypes::has first, since a program without the stdlib legitimately has no string
    std::optional<CoreStringLayout> resolve_core_string_layout(const CoreTypes &types, std::string &out_error);
};

#endif
