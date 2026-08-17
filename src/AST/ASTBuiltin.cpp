#include "AST/ASTBuiltin.h"

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
            { "needs_destruction", AST::BuiltinKind::t_needs_destruction },
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
        case AST::BuiltinKind::t_needs_destruction:
            return AST::BuiltinFoldability::t_ast_fact;

        // and the two that read a DataLayout. they still fold, at codegen, where there is one
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
            return AST::BuiltinFoldability::t_needs_layout;

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
        case AST::BuiltinKind::t_needs_destruction:
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
        case AST::BuiltinKind::t_needs_destruction:
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
        case AST::BuiltinKind::t_needs_destruction:
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
        case AST::BuiltinKind::t_needs_destruction:
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
            return false;
    }

    return false;
}
