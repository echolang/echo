#include "AST/ASTSimd.h"

#include <fmt/format.h>
#include <unordered_set>

namespace AST
{
    namespace
    {
        bool is_power_of_two(uint64_t n)
        {
            return n >= 2 && (n & (n - 1)) == 0;
        }

        bool lane_is_legal(const ValueType &element)
        {
            const ValueType bare = ValueType::make_mutable(element);

            if (!bare.is_primitive() || bare.is_void()) {
                return false;
            }

            return bare.is_integer_type() || bare.is_floating_type() || bare.is_boolean_type();
        }

        uint8_t lane_bytes(const ValueType &element)
        {
            const ValueType bare = ValueType::make_mutable(element);

            if (!bare.is_primitive()) {
                return 0;
            }

            return get_primitive_size(bare.get_primitive_type());
        }

        SimdShape this_simd_shape(const ValueType &type)
        {
            SimdShape result;

            const ValueType &element = type.simd_element();
            const ValueType &length = type.simd_length();

            if (is_undetermined_type(element) || contains_type_param(element)
                || length.is_type_param() || contains_type_param(length)) {
                result.kind = SimdShapeKind::t_pending;
                return result;
            }

            if (!lane_is_legal(element)) {
                result.kind = SimdShapeKind::t_refused;
                result.sentence = fmt::format(
                    "'simd<{}, {}>' is not a vector - a lane is an integer, a float or a bool, "
                    "and '{}' is none of those",
                    element.get_type_desciption(),
                    length.get_type_desciption(),
                    element.get_type_desciption());
                return result;
            }

            const std::optional<uint64_t> n = type.bound_simd_length();

            if (!n.has_value()) {
                result.kind = SimdShapeKind::t_pending;
                return result;
            }

            if (!is_power_of_two(*n)) {
                result.kind = SimdShapeKind::t_refused;
                result.sentence = fmt::format(
                    "'simd<{}, {}>' is not a vector - the lane count is a power of two, at least 2",
                    element.get_type_desciption(),
                    length.get_type_desciption());
                return result;
            }

            const uint8_t bytes = lane_bytes(element);
            const uint64_t width = static_cast<uint64_t>(bytes) * *n;

            if (width > k_simd_max_bytes) {
                result.kind = SimdShapeKind::t_refused;
                result.sentence = fmt::format(
                    "'simd<{}, {}>' is {} bytes, and a vector is at most {} - that is both "
                    "architectures' SIMD baseline and the heap allocator's alignment",
                    element.get_type_desciption(),
                    length.get_type_desciption(),
                    width,
                    k_simd_max_bytes);
                return result;
            }

            result.kind = SimdShapeKind::t_fine;
            return result;
        }

        SimdShape first_refused_or_pending(SimdShape current, const SimdShape &inner)
        {
            if (inner.kind == SimdShapeKind::t_refused) {
                return inner;
            }

            if (inner.kind == SimdShapeKind::t_pending
                && current.kind != SimdShapeKind::t_refused) {
                current.kind = SimdShapeKind::t_pending;
            }

            return current;
        }
    };

    SimdShape simd_shape_refusal(const ValueType &type)
    {
        const ValueType bare = ValueType::make_mutable(type);

        if (bare.is_pointer()) {
            return simd_shape_refusal(bare.pointee());
        }

        if (bare.is_wrapped_optional()) {
            return simd_shape_refusal(bare.optional_payload());
        }

        if (bare.is_inline_array()) {
            return simd_shape_refusal(bare.array_element());
        }

        SimdShape result;

        if (bare.has_complex_type()) {
            const ComplexType *ct = bare.get_complex_type();

            if (ct != nullptr) {
                for (const ValueType &arg : ct->instantiation_args) {
                    result = first_refused_or_pending(result, simd_shape_refusal(arg));

                    if (result.kind == SimdShapeKind::t_refused) {
                        return result;
                    }
                }
            }
        }

        if (bare.has_signature()) {
            const CallableSignature &sig = bare.signature();

            for (const ValueType &param : sig.parameter_types) {
                result = first_refused_or_pending(result, simd_shape_refusal(param));

                if (result.kind == SimdShapeKind::t_refused) {
                    return result;
                }
            }

            result = first_refused_or_pending(result, simd_shape_refusal(sig.return_type));

            if (result.kind == SimdShapeKind::t_refused) {
                return result;
            }
        }

        if (bare.is_simd()) {
            const SimdShape self = this_simd_shape(bare);

            if (self.kind == SimdShapeKind::t_refused) {
                return self;
            }

            return first_refused_or_pending(result, self);
        }

        return result;
    }

    namespace
    {
        std::optional<std::string> simd_in_c_layout(
            const ValueType &type,
            std::unordered_set<const ComplexType *> &seen)
        {
            const ValueType bare = ValueType::make_mutable(type);

            if (bare.is_simd()) {
                return fmt::format(
                    "'{}' has no C spelling - a vector's calling convention differs per platform. "
                    "Pass a 'ptr<{}>' instead.",
                    bare.get_type_desciption(),
                    bare.get_type_desciption());
            }

            // a pointer (and a borrow) is one word. the pointee's ABI is a different question,
            // asked of a `simd` by value. a class handle is the same: it is an address
            if (bare.is_pointer() || bare.is_weak() || bare.is_class()) {
                return std::nullopt;
            }

            if (bare.is_wrapped_optional()) {
                return simd_in_c_layout(bare.optional_payload(), seen);
            }

            if (bare.is_inline_array()) {
                return simd_in_c_layout(bare.array_element(), seen);
            }

            if (bare.has_signature()) {
                const CallableSignature &sig = bare.signature();

                if (auto inner = simd_in_c_layout(sig.return_type, seen)) {
                    return inner;
                }

                for (const ValueType &param : sig.parameter_types) {
                    if (auto inner = simd_in_c_layout(param, seen)) {
                        return inner;
                    }
                }

                return std::nullopt;
            }

            if (!bare.is_struct() && !bare.is_enum()) {
                return std::nullopt;
            }

            const ComplexType *ct = bare.get_complex_type();

            if (ct == nullptr || !seen.insert(ct).second) {
                return std::nullopt;
            }

            for (size_t i = 0; i < ct->property_count(); i++) {
                if (auto inner = simd_in_c_layout(ct->get_property_type(i), seen)) {
                    return inner;
                }
            }

            return std::nullopt;
        }
    };

    std::optional<std::string> simd_crosses_c_refusal(const ValueType &type)
    {
        std::unordered_set<const ComplexType *> seen;

        return simd_in_c_layout(type, seen);
    }

    bool is_simd_builtin(BuiltinKind kind)
    {
        // no tail, for builtin_message_index's reason
        switch (kind) {
            case BuiltinKind::t_simd_splat:
            case BuiltinKind::t_simd_load:
            case BuiltinKind::t_simd_store:
            case BuiltinKind::t_simd_select:
            case BuiltinKind::t_simd_bitmask:
            case BuiltinKind::t_simd_from_array:
            case BuiltinKind::t_simd_to_array:
                return true;

            case BuiltinKind::t_size_of:
            case BuiltinKind::t_align_of:
            case BuiltinKind::t_is_trivially_copyable:
            case BuiltinKind::t_is_integer:
            case BuiltinKind::t_needs_destruction:
            case BuiltinKind::t_integer_min:
            case BuiltinKind::t_integer_max:
            case BuiltinKind::t_type_id:
            case BuiltinKind::t_erased_from:
            case BuiltinKind::t_erased_retain:
            case BuiltinKind::t_erased_release:
            case BuiltinKind::t_assume:
            case BuiltinKind::t_take:
            case BuiltinKind::t_init:
            case BuiltinKind::t_die:
            case BuiltinKind::t_assert:
            case BuiltinKind::t_unwrap_abort:
            case BuiltinKind::t_crash_set_hook:
            case BuiltinKind::t_crash_take_hook:
            case BuiltinKind::t_crash_default_hook:
            case BuiltinKind::t_ref_count:
            case BuiltinKind::t_weak_count:
            case BuiltinKind::t_dprint:
            case BuiltinKind::t_alloc_bytes:
            case BuiltinKind::t_realloc_bytes:
            case BuiltinKind::t_free_bytes:
            case BuiltinKind::t_live_allocations:
            case BuiltinKind::t_process_argc:
            case BuiltinKind::t_process_argv:
            case BuiltinKind::t_process_envp:
            case BuiltinKind::t_exit:
            case BuiltinKind::t_atomic_load:
            case BuiltinKind::t_atomic_store:
            case BuiltinKind::t_atomic_add:
            case BuiltinKind::t_atomic_sub:
            case BuiltinKind::t_atomic_exchange:
            case BuiltinKind::t_atomic_compare_exchange:
            case BuiltinKind::t_atomic_fence:
                return false;
        }

        return false;
    }

    std::optional<std::string> simd_operand_refusal(
        BuiltinKind kind,
        const std::vector<ValueType> &bindings)
    {
        if (!is_simd_builtin(kind)) {
            return std::nullopt;
        }

        ValueType element;
        ValueType length;

        if (kind == BuiltinKind::t_simd_bitmask) {
            if (bindings.size() != 1) {
                return std::nullopt;
            }

            element = ValueType(ValueTypePrimitive::t_bool);
            length = bindings[0];
        }
        else {
            if (bindings.size() != 2) {
                return std::nullopt;
            }

            element = bindings[0];
            length = bindings[1];
        }

        const SimdShape shape = simd_shape_refusal(ValueType::make_simd(element, length));

        if (shape.kind != SimdShapeKind::t_refused) {
            return std::nullopt;
        }

        return shape.sentence;
    }
};
