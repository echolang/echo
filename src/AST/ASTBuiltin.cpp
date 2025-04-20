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
        // whatever it is handed, so there is nothing about it that has to be a literal
        case AST::BuiltinKind::t_size_of:
        case AST::BuiltinKind::t_align_of:
        case AST::BuiltinKind::t_ref_count:
        case AST::BuiltinKind::t_weak_count:
        case AST::BuiltinKind::t_dprint:
            return std::nullopt;
    }

    return std::nullopt;
}
