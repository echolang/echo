#include "AST/ASTAtomics.h"

#include <fmt/core.h>

namespace AST
{
    bool is_atomic_builtin(BuiltinKind kind)
    {
        // no tail, for builtin_message_index's reason
        switch (kind) {
            case BuiltinKind::t_atomic_load:
            case BuiltinKind::t_atomic_store:
            case BuiltinKind::t_atomic_add:
            case BuiltinKind::t_atomic_sub:
            case BuiltinKind::t_atomic_exchange:
            case BuiltinKind::t_atomic_compare_exchange:
            case BuiltinKind::t_atomic_fence:
                return true;

            case BuiltinKind::t_size_of:
            case BuiltinKind::t_align_of:
            case BuiltinKind::t_is_trivially_copyable:
            case BuiltinKind::t_needs_destruction:
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
                return false;
        }

        return false;
    }

    std::optional<std::string> atomic_operand_refusal(const ValueType &type, BuiltinKind kind)
    {
        // a fence has no operand. listed first so a call that somehow handed it a type still
        // answers "may" rather than inventing a sentence about a slot that is not there
        if (kind == BuiltinKind::t_atomic_fence) {
            return std::nullopt;
        }

        const bool rmw = kind == BuiltinKind::t_atomic_add || kind == BuiltinKind::t_atomic_sub;

        if (type.is_integer_type()) {
            return std::nullopt;
        }

        if (type.is_boolean_type()) {
            if (rmw) {
                return std::string(
                    "cannot atomically add or subtract a bool - there is no integer RMW on a flag. "
                    "Write load, store, exchange or compare_exchange.");
            }

            return std::nullopt;
        }

        if (type.is_pointer() || type.is_c_function()) {
            if (rmw) {
                return std::string(
                    "cannot atomically add to a pointer - that would move the address by bytes, "
                    "not by elements. Write load, store, exchange or compare_exchange.");
            }

            return std::nullopt;
        }

        if (type.is_floating_type()) {
            return fmt::format(
                "cannot atomically operate on '{}' - a floating add would not mean what integer "
                "add means.",
                type.get_type_desciption());
        }

        if (type.is_class() || type.is_weak() || type.is_interface()) {
            return fmt::format(
                "cannot atomically operate on '{}' - an exchange would move the bits without "
                "moving the count. Put it in a mutex, or mark the class '#[atomic]' and copy the "
                "handle.",
                type.get_type_desciption());
        }

        if (type.is_struct() || type.is_enum() || type.is_callable()) {
            return fmt::format(
                "cannot atomically operate on '{}' - it is wider than a word, and an exchange "
                "would not retain. Put it in a mutex.",
                type.get_type_desciption());
        }

        return fmt::format("cannot atomically operate on '{}'.", type.get_type_desciption());
    }
};
