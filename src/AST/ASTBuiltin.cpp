#include "AST/ASTBuiltin.h"

#include "AST/ASTCollector.h"
#include "AST/ASTVisibility.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeNode.h"

#include <cassert>
#include <optional>
#include <unordered_map>

namespace
{
    const std::unordered_map<std::string, AST::BuiltinKind> &builtin_table()
    {
        static const std::unordered_map<std::string, AST::BuiltinKind> table = {
            { "size_of", AST::BuiltinKind::t_size_of },
            { "align_of", AST::BuiltinKind::t_align_of },
            { "is_trivially_copyable", AST::BuiltinKind::t_is_trivially_copyable },
            { "is_integer", AST::BuiltinKind::t_is_integer },
            { "needs_destruction", AST::BuiltinKind::t_needs_destruction },
            { "integer_min", AST::BuiltinKind::t_integer_min },
            { "integer_max", AST::BuiltinKind::t_integer_max },
            { "type_id", AST::BuiltinKind::t_type_id },
            { "erased_from", AST::BuiltinKind::t_erased_from },
            { "erased_retain", AST::BuiltinKind::t_erased_retain },
            { "erased_release", AST::BuiltinKind::t_erased_release },
            { "assume", AST::BuiltinKind::t_assume },
            { "take", AST::BuiltinKind::t_take },
            { "init", AST::BuiltinKind::t_init },
            { "die", AST::BuiltinKind::t_die },
            { "assert", AST::BuiltinKind::t_assert },
            { "unwrap_abort", AST::BuiltinKind::t_unwrap_abort },
            { "crash_set_hook", AST::BuiltinKind::t_crash_set_hook },
            { "crash_take_hook", AST::BuiltinKind::t_crash_take_hook },
            { "crash_default_hook", AST::BuiltinKind::t_crash_default_hook },
            { "ref_count", AST::BuiltinKind::t_ref_count },
            { "weak_count", AST::BuiltinKind::t_weak_count },
            { "dprint", AST::BuiltinKind::t_dprint },
            { "alloc_bytes", AST::BuiltinKind::t_alloc_bytes },
            { "realloc_bytes", AST::BuiltinKind::t_realloc_bytes },
            { "free_bytes", AST::BuiltinKind::t_free_bytes },
            { "live_allocations", AST::BuiltinKind::t_live_allocations },
            { "process_argc", AST::BuiltinKind::t_process_argc },
            { "process_argv", AST::BuiltinKind::t_process_argv },
            { "process_envp", AST::BuiltinKind::t_process_envp },
            { "exit", AST::BuiltinKind::t_exit },
            { "atomic_load", AST::BuiltinKind::t_atomic_load },
            { "atomic_store", AST::BuiltinKind::t_atomic_store },
            { "atomic_add", AST::BuiltinKind::t_atomic_add },
            { "atomic_sub", AST::BuiltinKind::t_atomic_sub },
            { "atomic_exchange", AST::BuiltinKind::t_atomic_exchange },
            { "atomic_compare_exchange", AST::BuiltinKind::t_atomic_compare_exchange },
            { "atomic_fence", AST::BuiltinKind::t_atomic_fence },
            { "simd_splat", AST::BuiltinKind::t_simd_splat },
            { "simd_load", AST::BuiltinKind::t_simd_load },
            { "simd_store", AST::BuiltinKind::t_simd_store },
            { "simd_select", AST::BuiltinKind::t_simd_select },
            { "simd_bitmask", AST::BuiltinKind::t_simd_bitmask },
            { "simd_from_array", AST::BuiltinKind::t_simd_from_array },
            { "simd_to_array", AST::BuiltinKind::t_simd_to_array },
        };
        return table;
    }
}

bool AST::is_known_builtin(const std::string &name)
{
    return builtin_table().find(name) != builtin_table().end();
}

AST::BuiltinKind AST::builtin_kind_for(const std::string &name)
{
    auto it = builtin_table().find(name);
    assert(it != builtin_table().end() && "builtin_kind_for called with a name is_known_builtin rejected");
    return it->second;
}

AST::BuiltinFoldability AST::builtin_foldability(AST::BuiltinKind kind)
{
    // no tail, for builtin_message_index's reason
    switch (kind) {
        // the two AST facts. AST::const_fold owns them now and ExprCodegen asks it, so the answer has
        // one spelling rather than two held in step by nothing
        case AST::BuiltinKind::t_is_trivially_copyable:
        case AST::BuiltinKind::t_is_integer:
        case AST::BuiltinKind::t_needs_destruction:
        case AST::BuiltinKind::t_integer_min:
        case AST::BuiltinKind::t_integer_max:
            return AST::BuiltinFoldability::t_ast_fact;

        // and the two that read a DataLayout. they still fold, at codegen, where there is one
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
            return AST::BuiltinFoldability::t_needs_layout;

        case AST::BuiltinKind::t_type_id:
        case AST::BuiltinKind::t_erased_from:
        case AST::BuiltinKind::t_erased_retain:
        case AST::BuiltinKind::t_erased_release:
        case AST::BuiltinKind::t_assume:
            return AST::BuiltinFoldability::t_not_a_query;

        // everything else does something rather than answering something. `take` moves a value out of
        // a place and `init` moves one in, `ref_count` and `weak_count` read a word of a live heap block,
        // `dprint` prints, the raw-memory trio allocates, `live_allocations` reads a counter the program
        // maintains, the three process accessors read globals `main` filled in, and `die`/`assert`/`exit`
        // stop
        case AST::BuiltinKind::t_take:
        case AST::BuiltinKind::t_init:
        case AST::BuiltinKind::t_die:
        case AST::BuiltinKind::t_assert:
        case AST::BuiltinKind::t_unwrap_abort:
        case AST::BuiltinKind::t_crash_set_hook:
        case AST::BuiltinKind::t_crash_take_hook:
        case AST::BuiltinKind::t_crash_default_hook:
        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
        case AST::BuiltinKind::t_dprint:
        case AST::BuiltinKind::t_alloc_bytes:
        case AST::BuiltinKind::t_realloc_bytes:
        case AST::BuiltinKind::t_free_bytes:
        case AST::BuiltinKind::t_live_allocations:
        case AST::BuiltinKind::t_process_argc:
        case AST::BuiltinKind::t_process_argv:
        case AST::BuiltinKind::t_process_envp:
        case AST::BuiltinKind::t_exit:
        case AST::BuiltinKind::t_atomic_load:
        case AST::BuiltinKind::t_atomic_store:
        case AST::BuiltinKind::t_atomic_add:
        case AST::BuiltinKind::t_atomic_sub:
        case AST::BuiltinKind::t_atomic_exchange:
        case AST::BuiltinKind::t_atomic_compare_exchange:
        case AST::BuiltinKind::t_atomic_fence:
        case AST::BuiltinKind::t_simd_splat:
        case AST::BuiltinKind::t_simd_load:
        case AST::BuiltinKind::t_simd_store:
        case AST::BuiltinKind::t_simd_select:
        case AST::BuiltinKind::t_simd_bitmask:
        case AST::BuiltinKind::t_simd_from_array:
        case AST::BuiltinKind::t_simd_to_array:
            return AST::BuiltinFoldability::t_not_a_query;
    }

    return AST::BuiltinFoldability::t_not_a_query;
}

bool AST::builtin_never_returns(AST::BuiltinKind kind)
{
    // no tail, for builtin_message_index's reason
    switch (kind) {
        // `assert` is deliberately not here: it returns when it holds
        case AST::BuiltinKind::t_die:
        case AST::BuiltinKind::t_unwrap_abort:
        case AST::BuiltinKind::t_exit:
            return true;

        case AST::BuiltinKind::t_assert:
        case AST::BuiltinKind::t_crash_set_hook:
        case AST::BuiltinKind::t_crash_take_hook:
        case AST::BuiltinKind::t_crash_default_hook:
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
        case AST::BuiltinKind::t_is_trivially_copyable:
        case AST::BuiltinKind::t_is_integer:
        case AST::BuiltinKind::t_needs_destruction:
        case AST::BuiltinKind::t_integer_min:
        case AST::BuiltinKind::t_integer_max:
        case AST::BuiltinKind::t_type_id:
        case AST::BuiltinKind::t_erased_from:
        case AST::BuiltinKind::t_erased_retain:
        case AST::BuiltinKind::t_erased_release:
        case AST::BuiltinKind::t_assume:
        case AST::BuiltinKind::t_take:
        case AST::BuiltinKind::t_init:
        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
        case AST::BuiltinKind::t_dprint:
        case AST::BuiltinKind::t_alloc_bytes:
        case AST::BuiltinKind::t_realloc_bytes:
        case AST::BuiltinKind::t_free_bytes:
        case AST::BuiltinKind::t_live_allocations:
        case AST::BuiltinKind::t_process_argc:
        case AST::BuiltinKind::t_process_argv:
        case AST::BuiltinKind::t_process_envp:
        case AST::BuiltinKind::t_atomic_load:
        case AST::BuiltinKind::t_atomic_store:
        case AST::BuiltinKind::t_atomic_add:
        case AST::BuiltinKind::t_atomic_sub:
        case AST::BuiltinKind::t_atomic_exchange:
        case AST::BuiltinKind::t_atomic_compare_exchange:
        case AST::BuiltinKind::t_atomic_fence:
        case AST::BuiltinKind::t_simd_splat:
        case AST::BuiltinKind::t_simd_load:
        case AST::BuiltinKind::t_simd_store:
        case AST::BuiltinKind::t_simd_select:
        case AST::BuiltinKind::t_simd_bitmask:
        case AST::BuiltinKind::t_simd_from_array:
        case AST::BuiltinKind::t_simd_to_array:
            return false;
    }

    return false;
}

bool AST::builtin_owns_raw_storage(AST::BuiltinKind kind)
{
    // no tail, for builtin_message_index's reason - and here the silent answer would be the unsafe
    // one: a builtin added without an arm would keep the `unsafe` rule rather than escape it
    switch (kind) {
        case AST::BuiltinKind::t_take:
        case AST::BuiltinKind::t_init:
            return true;

        // these three take an ordinary `T&`, which is exactly why they are named: as "every builtin"
        // they were exempt from the promotion rule and had no business being
        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
        case AST::BuiltinKind::t_dprint:

        case AST::BuiltinKind::t_die:
        case AST::BuiltinKind::t_unwrap_abort:
        case AST::BuiltinKind::t_exit:
        case AST::BuiltinKind::t_assert:
        case AST::BuiltinKind::t_crash_set_hook:
        case AST::BuiltinKind::t_crash_take_hook:
        case AST::BuiltinKind::t_crash_default_hook:
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
        case AST::BuiltinKind::t_is_trivially_copyable:
        case AST::BuiltinKind::t_is_integer:
        case AST::BuiltinKind::t_needs_destruction:
        case AST::BuiltinKind::t_integer_min:
        case AST::BuiltinKind::t_integer_max:
        case AST::BuiltinKind::t_type_id:
        case AST::BuiltinKind::t_erased_from:
        case AST::BuiltinKind::t_erased_retain:
        case AST::BuiltinKind::t_erased_release:
        case AST::BuiltinKind::t_assume:
        case AST::BuiltinKind::t_alloc_bytes:
        case AST::BuiltinKind::t_realloc_bytes:
        case AST::BuiltinKind::t_free_bytes:
        case AST::BuiltinKind::t_live_allocations:
        case AST::BuiltinKind::t_process_argc:
        case AST::BuiltinKind::t_process_argv:
        case AST::BuiltinKind::t_process_envp:
        case AST::BuiltinKind::t_atomic_load:
        case AST::BuiltinKind::t_atomic_store:
        case AST::BuiltinKind::t_atomic_add:
        case AST::BuiltinKind::t_atomic_sub:
        case AST::BuiltinKind::t_atomic_exchange:
        case AST::BuiltinKind::t_atomic_compare_exchange:
        case AST::BuiltinKind::t_atomic_fence:
        case AST::BuiltinKind::t_simd_splat:
        case AST::BuiltinKind::t_simd_load:
        case AST::BuiltinKind::t_simd_store:
        case AST::BuiltinKind::t_simd_select:
        case AST::BuiltinKind::t_simd_bitmask:
        case AST::BuiltinKind::t_simd_from_array:
        case AST::BuiltinKind::t_simd_to_array:
            return false;
    }

    return false;
}

std::optional<size_t> AST::builtin_message_index(AST::BuiltinKind kind)
{
    // no tail, deliberately: a builtin added without an arm here is a compile error rather than one
    // that silently accepts a message it cannot fold
    switch (kind) {
        case AST::BuiltinKind::t_die:
            return 0;

        // behind the condition
        case AST::BuiltinKind::t_assert:
            return 1;

        // nothing to fold: size_of, align_of and the two ownership predicates take no arguments at all,
        // `take`'s one argument is the place it is emptying and `init`'s two are that place and the
        // value going into it, the two counts' one argument
        // is a class handle rather than a message, and dprint's is the value being printed - it renders
        // whatever it is handed, so there is nothing about it that has to be a literal. the raw-memory
        // trio takes sizes and addresses, and live_allocations takes nothing. the three process
        // accessors take nothing either, and `exit` takes a code rather than a message - it is the one
        // way of stopping that prints *nothing*, which is what separates it from `die`
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
        case AST::BuiltinKind::t_is_trivially_copyable:
        case AST::BuiltinKind::t_is_integer:
        case AST::BuiltinKind::t_needs_destruction:
        case AST::BuiltinKind::t_integer_min:
        case AST::BuiltinKind::t_integer_max:
        case AST::BuiltinKind::t_type_id:
        case AST::BuiltinKind::t_erased_from:
        case AST::BuiltinKind::t_erased_retain:
        case AST::BuiltinKind::t_erased_release:
        case AST::BuiltinKind::t_assume:
        case AST::BuiltinKind::t_take:
        case AST::BuiltinKind::t_init:
        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
        case AST::BuiltinKind::t_dprint:
        case AST::BuiltinKind::t_alloc_bytes:
        case AST::BuiltinKind::t_realloc_bytes:
        case AST::BuiltinKind::t_free_bytes:
        case AST::BuiltinKind::t_live_allocations:
        case AST::BuiltinKind::t_process_argc:
        case AST::BuiltinKind::t_process_argv:
        case AST::BuiltinKind::t_process_envp:
        case AST::BuiltinKind::t_unwrap_abort:
        case AST::BuiltinKind::t_crash_set_hook:
        case AST::BuiltinKind::t_crash_take_hook:
        case AST::BuiltinKind::t_crash_default_hook:
        case AST::BuiltinKind::t_exit:
        case AST::BuiltinKind::t_atomic_load:
        case AST::BuiltinKind::t_atomic_store:
        case AST::BuiltinKind::t_atomic_add:
        case AST::BuiltinKind::t_atomic_sub:
        case AST::BuiltinKind::t_atomic_exchange:
        case AST::BuiltinKind::t_atomic_compare_exchange:
        case AST::BuiltinKind::t_atomic_fence:
        case AST::BuiltinKind::t_simd_splat:
        case AST::BuiltinKind::t_simd_load:
        case AST::BuiltinKind::t_simd_store:
        case AST::BuiltinKind::t_simd_select:
        case AST::BuiltinKind::t_simd_bitmask:
        case AST::BuiltinKind::t_simd_from_array:
        case AST::BuiltinKind::t_simd_to_array:
            return std::nullopt;
    }

    return std::nullopt;
}

bool AST::builtin_message_must_be_literal(AST::BuiltinKind kind)
{
    // no tail, for builtin_message_index's reason: a builtin added without an arm would silently
    // accept a runtime message that the call then compiled out
    switch (kind) {
        case AST::BuiltinKind::t_assert:
            return true;

        case AST::BuiltinKind::t_die:
        case AST::BuiltinKind::t_unwrap_abort:
        case AST::BuiltinKind::t_exit:
        case AST::BuiltinKind::t_crash_set_hook:
        case AST::BuiltinKind::t_crash_take_hook:
        case AST::BuiltinKind::t_crash_default_hook:
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
        case AST::BuiltinKind::t_is_trivially_copyable:
        case AST::BuiltinKind::t_is_integer:
        case AST::BuiltinKind::t_needs_destruction:
        case AST::BuiltinKind::t_integer_min:
        case AST::BuiltinKind::t_integer_max:
        case AST::BuiltinKind::t_type_id:
        case AST::BuiltinKind::t_erased_from:
        case AST::BuiltinKind::t_erased_retain:
        case AST::BuiltinKind::t_erased_release:
        case AST::BuiltinKind::t_assume:
        case AST::BuiltinKind::t_take:
        case AST::BuiltinKind::t_init:
        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
        case AST::BuiltinKind::t_dprint:
        case AST::BuiltinKind::t_alloc_bytes:
        case AST::BuiltinKind::t_realloc_bytes:
        case AST::BuiltinKind::t_free_bytes:
        case AST::BuiltinKind::t_live_allocations:
        case AST::BuiltinKind::t_process_argc:
        case AST::BuiltinKind::t_process_argv:
        case AST::BuiltinKind::t_process_envp:
        case AST::BuiltinKind::t_atomic_load:
        case AST::BuiltinKind::t_atomic_store:
        case AST::BuiltinKind::t_atomic_add:
        case AST::BuiltinKind::t_atomic_sub:
        case AST::BuiltinKind::t_atomic_exchange:
        case AST::BuiltinKind::t_atomic_compare_exchange:
        case AST::BuiltinKind::t_atomic_fence:
        case AST::BuiltinKind::t_simd_splat:
        case AST::BuiltinKind::t_simd_load:
        case AST::BuiltinKind::t_simd_store:
        case AST::BuiltinKind::t_simd_select:
        case AST::BuiltinKind::t_simd_bitmask:
        case AST::BuiltinKind::t_simd_from_array:
        case AST::BuiltinKind::t_simd_to_array:
            return false;
    }

    return false;
}

uint64_t AST::integer_bound_bits(ValueTypePrimitive primitive, bool is_max)
{
    // IntegerSize is the one extrema table: a literal's range check already reads it, so
    // `usize::max()` cannot drift from what `18446744073709551615 as usize` is allowed to be
    const IntegerSize size = get_integer_size(primitive);

    if (is_max) {
        return size.get_max_positive_value();
    }

    return static_cast<uint64_t>(size.get_max_negative_value());
}

AST::FunctionDeclNode *AST::integer_bound_decl(
    Collector &collector,
    const ValueType &type,
    const std::string &name,
    const TokenReference &at
)
{
    if (!type.is_integer_type() || (name != "min" && name != "max")) {
        return nullptr;
    }

    const bool is_max = name == "max";
    const uint32_t key =
        (static_cast<uint32_t>(type.get_primitive_type()) << 1) | (is_max ? 1u : 0u);

    if (auto found = collector._integer_bound_decls.find(key); found != collector._integer_bound_decls.end()) {
        return found->second;
    }

    auto &decl = collector._compiler_nodes.emplace_back<FunctionDeclNode>(at);
    decl.builtin = is_max ? "integer_max" : "integer_min";
    decl.is_implicitly_generated = true;
    decl.visibility = Visibility::t_public;
    decl.instantiation_args = { ValueType(type.get_primitive_type()) };

    auto &return_type = collector._compiler_nodes.emplace_back<TypeNode>(
        ValueType(type.get_primitive_type())
    );
    decl.return_type = &return_type;

    collector._integer_bound_decls[key] = &decl;
    return &decl;
}
