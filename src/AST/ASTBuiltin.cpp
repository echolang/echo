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
