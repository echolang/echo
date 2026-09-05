#include "AST/ASTCompleteness.h"

#include <fmt/format.h>

namespace AST
{
    TypeCompleteness type_completeness(const ValueType &type)
    {
        // a tagged optional is complete exactly when its payload is: you cannot lay out
        // `{ __has, __value }` over a type that has no size
        if (type.is_wrapped_optional()) {
            return type_completeness(type.optional_payload());
        }

        // `T[N]` is complete exactly when T is. asked before the pointer arm so a
        // `Handle[4]` is incomplete rather than "an array, therefore a value"
        if (type.is_inline_array()) {
            return type_completeness(type.array_element());
        }

        // a pointer (and a borrow) is one word, whatever it names. the pointee's completeness
        // is a different question, asked of the pointee
        if (type.is_pointer() || type.is_weak() || type.is_c_function() || type.is_callable()) {
            return TypeCompleteness::t_complete;
        }

        if (type.is_unknown() || type.is_type_param() || contains_type_param(type)) {
            return TypeCompleteness::t_pending;
        }

        if (type.is_opaque()) {
            return TypeCompleteness::t_incomplete;
        }

        return TypeCompleteness::t_complete;
    }

    std::optional<std::string> incomplete_use_refusal(const ValueType &type)
    {
        // a nullable pointer is a value of one word even when the pointee is incomplete -
        // that is the whole of `ptr<Handle>`. a borrow is not: `T&` is Echo-accounted
        // storage, and an incomplete type has none
        if (type.is_pointer()) {
            if (!type.is_nullable()
                && type_completeness(type.pointee()) == TypeCompleteness::t_incomplete) {
                const ValueType &pointee = type.pointee();
                return fmt::format(
                    "'{}' is an incomplete type, so it cannot be borrowed - a borrow names "
                    "storage Echo accounts for, and an incomplete type has none. Write "
                    "'ptr<{}>' for a C handle.",
                    pointee.get_type_desciption(), pointee.get_type_desciption());
            }

            return std::nullopt;
        }

        if (type.is_inline_array()) {
            return incomplete_use_refusal(type.array_element());
        }

        if (type.is_wrapped_optional()) {
            return incomplete_use_refusal(type.optional_payload());
        }

        if (type_completeness(type) != TypeCompleteness::t_incomplete) {
            return std::nullopt;
        }

        return fmt::format(
            "'{}' is an incomplete type, so a value of it cannot exist - name it only as "
            "'ptr<{}>'.",
            type.get_type_desciption(), type.get_type_desciption());
    }

    std::optional<std::string> incomplete_stride_refusal(const ValueType &pointer)
    {
        if (!pointer.is_pointer()) {
            return std::nullopt;
        }

        if (type_completeness(pointer.pointee()) != TypeCompleteness::t_incomplete) {
            return std::nullopt;
        }

        return fmt::format(
            "cannot offset a pointer to incomplete type '{}' - the element size is not known",
            pointer.pointee().get_type_desciption());
    }
};
