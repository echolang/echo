#include "AST/ASTFileRoot.h"

#include "AST/ASTNodeReference.h"
#include "AST/ScopeNode.h"

namespace
{
    // the shapes a file scope can hold that emit nothing where they stand.
    //
    // `has_type` is not used here because the tag is the whole of the question - a node reference carries
    // its tag beside its pointer, and a parser error arm can leave the pointer null with the tag set. A
    // half-parsed declaration is still a declaration for this purpose, and refusing it as top-level code
    // would report the wrong thing about a file that has already failed to parse
    bool is_declaration(const AST::NodeReference &statement)
    {
        switch (statement.type()) {
        case AST::NodeType::n_func_decl:
        case AST::NodeType::n_type_decl:
        case AST::NodeType::n_const_decl:
        case AST::NodeType::n_attribute:
        case AST::NodeType::n_namespace_decl:
        case AST::NodeType::n_use_decl:
        case AST::NodeType::n_namespace:
            return true;

        default:
            return false;
        }
    }
};

AST::Node *AST::first_top_level_statement(const AST::ScopeNode &root)
{
    for (const AST::NodeReference &child : root.children) {
        if (is_declaration(child)) {
            continue;
        }

        return child.node();
    }

    return nullptr;
}
