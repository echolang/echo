#include "Compiler/Lsp/LspResolve.h"

#include "AST/ASTBundle.h"
#include "AST/ASTConstantExpander.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTNode.h"
#include "AST/ASTSourceToken.h"
#include "AST/ConstDeclNode.h"
#include "AST/ConstRefExprNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "Compiler/Lsp/LspUri.h"

namespace
{
    AST::VarDeclNode *property_decl_of(AST::Bundle &bundle, const AST::MemberAccessNode &access)
    {
        const AST::ValueType base = access.base_target_type();
        if (!base.has_complex_type()) {
            return nullptr;
        }

        AST::TypeDeclNode *owner = Compiler::Lsp::type_decl_of(bundle, base);
        if (owner == nullptr) {
            return nullptr;
        }

        const std::string &name = access.get_member_name().value();
        for (AST::VarDeclNode *prop : owner->properties()) {
            if (prop->name() == name) {
                return prop;
            }
        }

        return nullptr;
    }

    AST::ConstDeclNode *constant_decl_of(AST::Bundle &bundle, AST::ConstRefExprNode &ref)
    {
        return AST::find_constant(
            bundle.collector.namespaces,
            ref.lookup_name(),
            ref.lookup_namespace,
            ref.is_qualified);
    }
};

AST::TypeDeclNode *Compiler::Lsp::type_decl_of(AST::Bundle &bundle, const AST::ValueType &type)
{
    const AST::ValueType named = AST::target_type_of(type);
    if (!named.has_complex_type()) {
        return nullptr;
    }

    const AST::ComplexType *wanted = named.get_complex_type()->template_or_self();

    for (auto &module_ptr : bundle.modules) {
        for (AST::TypeDeclNode *decl : module_ptr->nodes.of_type<AST::TypeDeclNode>()) {
            if (&decl->complex_type() == wanted) {
                return decl;
            }
        }
    }

    return nullptr;
}

AST::Node *Compiler::Lsp::definition_target(AST::Node *node, AST::Bundle &bundle)
{
    if (node == nullptr) {
        return nullptr;
    }

    const AST::NodeReference ref = AST::make_ref(node);

    if (ref.has_type<AST::VarNode>()) {
        return &ref.get_ptr<AST::VarNode>()->decl();
    }

    if (ref.has_type<AST::FunctionCallExprNode>()) {
        return ref.get_ptr<AST::FunctionCallExprNode>()->decl;
    }

    if (ref.has_type<AST::TypeNode>()) {
        return type_decl_of(bundle, ref.get_ptr<AST::TypeNode>()->type);
    }

    if (ref.has_type<AST::MemberAccessNode>()) {
        return property_decl_of(bundle, *ref.get_ptr<AST::MemberAccessNode>());
    }

    if (ref.has_type<AST::StaticPropertyExprNode>()) {
        return ref.get_ptr<AST::StaticPropertyExprNode>()->decl;
    }

    if (ref.has_type<AST::FunctionRefExprNode>()) {
        return ref.get_ptr<AST::FunctionRefExprNode>()->decl;
    }

    if (ref.has_type<AST::ConstRefExprNode>()) {
        return constant_decl_of(bundle, *ref.get_ptr<AST::ConstRefExprNode>());
    }

    return node;
}

AST::Node *Compiler::Lsp::canonical_target(AST::Node *node)
{
    if (node == nullptr) {
        return nullptr;
    }

    const AST::NodeReference ref = AST::make_ref(node);
    if (ref.has_type<AST::FunctionDeclNode>()) {
        AST::FunctionDeclNode *fn = ref.get_ptr<AST::FunctionDeclNode>();
        if (fn->template_ref != nullptr) {
            return fn->template_ref;
        }
    }

    return node;
}

AST::Node *Compiler::Lsp::reference_target(
    AST::Node *node,
    AST::Bundle &bundle,
    std::unordered_map<AST::Node *, AST::Node *> &cache
)
{
    auto found = cache.find(node);
    if (found != cache.end()) {
        return found->second;
    }

    AST::Node *target = canonical_target(definition_target(node, bundle));
    cache[node] = target;
    return target;
}

const TokenReference *Compiler::Lsp::name_token_of(AST::Node *node)
{
    const AST::NodeReference ref = AST::make_ref(node);

    if (ref.has_type<AST::VarDeclNode>()) {
        return &ref.get_ptr<AST::VarDeclNode>()->token_varname;
    }

    if (ref.has_type<AST::FunctionDeclNode>()) {
        AST::FunctionDeclNode *fn = ref.get_ptr<AST::FunctionDeclNode>();
        if (fn->name_token.has_value()) {
            return &fn->name_token.value();
        }

        return nullptr;
    }

    if (ref.has_type<AST::TypeDeclNode>()) {
        AST::TypeDeclNode *type = ref.get_ptr<AST::TypeDeclNode>();
        if (type->name_token.has_value()) {
            return &type->name_token.value();
        }

        return nullptr;
    }

    if (ref.has_type<AST::ConstDeclNode>()) {
        return &ref.get_ptr<AST::ConstDeclNode>()->token_name;
    }

    if (ref.has_type<AST::ConstRefExprNode>()) {
        return &ref.get_ptr<AST::ConstRefExprNode>()->token_name;
    }

    return AST::source_token_of(*node);
}

bool Compiler::Lsp::is_stdlib_file(const AST::File &file)
{
    return is_embedded_stdlib_path(file.get_path())
        || (file.module != nullptr && file.module->name == "stdlib");
}
