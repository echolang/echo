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
            { "die", AST::BuiltinKind::t_die },
            { "assert", AST::BuiltinKind::t_assert },
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

bool AST::builtin_never_returns(AST::BuiltinKind kind)
{
    // no tail, for builtin_message_index's reason
    switch (kind) {
        // `assert` is deliberately not here: it returns when it holds
        case AST::BuiltinKind::t_die:
        case AST::BuiltinKind::t_exit:
            return true;

        case AST::BuiltinKind::t_assert:
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
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

        // nothing to fold: size_of and align_of take no arguments at all, the two counts' one argument
        // is a class handle rather than a message, and dprint's is the value being printed - it renders
        // whatever it is handed, so there is nothing about it that has to be a literal. the raw-memory
        // trio takes sizes and addresses, and live_allocations takes nothing. the three process
        // accessors take nothing either, and `exit` takes a code rather than a message - it is the one
        // way of stopping that prints *nothing*, which is what separates it from `die`
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
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
            return std::nullopt;
    }

    return std::nullopt;
}
