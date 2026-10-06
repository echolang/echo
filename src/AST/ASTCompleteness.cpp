#include "AST/ASTCompleteness.h"

#include <fmt/format.h>

#include <vector>

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

        // a vector is complete exactly when N is bound and T is. a pending N is the
        // template; an unbound T is too
        if (type.is_simd()) {
            if (!type.bound_simd_length().has_value()) {
                return TypeCompleteness::t_pending;
            }

            return type_completeness(type.simd_element());
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

        if (type.is_simd()) {
            return incomplete_use_refusal(type.simd_element());
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

namespace
{
    bool path_holds(const std::vector<AST::ValueType> &path, const AST::ComplexType *ct)
    {
        for (const AST::ValueType &step : path) {
            if (step.has_complex_type() && step.get_complex_type() == ct) {
                return true;
            }
        }

        return false;
    }

    const AST::ComplexType *layout_complex(const AST::ValueType &type)
    {
        if (type.is_inline_array()) {
            return layout_complex(type.array_element());
        }

        if (type.is_wrapped_optional()) {
            return layout_complex(type.optional_payload());
        }

        if (type.has_complex_type()) {
            return type.get_complex_type();
        }

        return nullptr;
    }

    AST::ValueType peel_wrappers(const AST::ValueType &type)
    {
        if (type.is_wrapped_optional()) {
            return peel_wrappers(type.optional_payload());
        }

        if (type.is_inline_array()) {
            return peel_wrappers(type.array_element());
        }

        return type;
    }

    std::string cycle_sentence(
        const std::vector<AST::ValueType> &path,
        const AST::ValueType &closing
    )
    {
        AST::ValueType named = peel_wrappers(closing);

        for (const AST::ValueType &step : path) {
            if (step.is_struct() || step.is_enum()) {
                named = step;
                break;
            }
        }

        return fmt::format(
            "'{}' contains itself through '{}' - a struct field is stored inline, so a cycle "
            "has no size. Use a class, or 'ptr<{}>'.",
            named.get_type_desciption(),
            closing.get_type_desciption(),
            named.get_type_desciption());
    }

    std::optional<std::string> walk_layout(
        const AST::ValueType &type,
        std::vector<AST::ValueType> &path
    )
    {
        // T itself has no layout until it is bound. Rec<T> is a named layout and is walked
        if (type.is_unknown() || type.is_type_param()) {
            return std::nullopt;
        }

        // peel wrappers the same way type_completeness does. if the payload is already on the
        // path, the wrapper is what closed the cycle (`Node?`, `Node[2]`)
        if (type.is_inline_array() || type.is_wrapped_optional()) {
            const AST::ValueType &inner =
                type.is_inline_array() ? type.array_element() : type.optional_payload();
            if (const AST::ComplexType *ct = layout_complex(inner);
                ct != nullptr && path_holds(path, ct)) {
                return cycle_sentence(path, type);
            }

            return walk_layout(inner, path);
        }

        // one word, or no by-value layout to walk. a class handle is the linked-list spelling
        if (type.is_pointer() || type.is_class() || type.is_weak() || type.is_c_function()
            || type.is_callable() || type.is_interface() || type.is_opaque()
            || type.is_simd() || type.is_primitive() || type.is_void()) {
            return std::nullopt;
        }

        if (!type.has_property_layout()) {
            return std::nullopt;
        }

        AST::ComplexType *ct = type.get_complex_type();
        if (ct == nullptr) {
            return std::nullopt;
        }

        if (path_holds(path, ct)) {
            return cycle_sentence(path, type);
        }

        path.push_back(type);
        std::optional<std::string> refusal;
        for (size_t i = 0; i < ct->property_count(); i++) {
            refusal = walk_layout(ct->get_property_type(i), path);
            if (refusal.has_value()) {
                break;
            }
        }
        path.pop_back();
        return refusal;
    }
};

std::optional<std::string> AST::layout_cycle_refusal(const ValueType &type)
{
    std::vector<ValueType> path;

    return walk_layout(type, path);
}
