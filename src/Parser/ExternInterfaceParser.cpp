#include "Parser/ExternInterfaceParser.h"

#include "AST/ASTCoreTypes.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTNodeReference.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/ASTExternInterface.h"
#include "AST/ASTVisibility.h"

#include <fmt/core.h>

namespace
{
AST::VarDeclNode *declare_slot(
    Parser::Payload &payload,
    AST::TypeDeclNode *type,
    const std::string &name,
    const AST::ValueType &slot_type,
    const TokenReference &at
)
{
    auto property_token = payload.context.make_virtual_token(
        "$" + name, Token::Type::t_varname, at);

    auto *property_type = payload.context.emplace_nodep<AST::TypeNode>(slot_type);
    auto *property = payload.context.emplace_nodep<AST::VarDeclNode>(property_token, property_type);
    property->visibility = AST::Visibility::t_owner;
    type->add_property(property);
    return property;
}

void plant_forwarder_body(
    Parser::Payload &payload,
    AST::FunctionDeclNode &requirement,
    const std::string &slot_name,
    const TokenReference &at
)
{
    requirement.body = &payload.context.emplace_node<AST::ScopeNode>();
    requirement.is_implicitly_generated = true;

    if (requirement.args.empty()) {
        return;
    }

    auto *this_var = payload.context.emplace_nodep<AST::VarNode>(requirement.args[0]);
    auto *this_read = payload.context.emplace_nodep<AST::VarRefNode>(this_var);

    auto slot_token = payload.context.make_virtual_token(slot_name, Token::Type::t_identifier, at);
    auto &slot = payload.context.emplace_node<AST::MemberAccessNode>(
        AST::make_ref(this_read), slot_token);

    std::vector<AST::ExprNode *> arguments;

    for (size_t i = 1; i < requirement.args.size(); ++i) {
        auto *arg_var = payload.context.emplace_nodep<AST::VarNode>(requirement.args[i]);
        arguments.push_back(payload.context.emplace_nodep<AST::VarRefNode>(arg_var));
    }

    auto &call = payload.context.emplace_node<AST::IndirectCallExprNode>(
        &slot, std::move(arguments), at);

    requirement.body->children.push_back(
        AST::make_ref(payload.context.emplace_node<AST::ReturnNode>(&call, at)));
}
}

void Parser::finalize_extern_interface(
    Payload &payload,
    AST::TypeDeclNode *type
)
{
    if (type == nullptr || !type->complex_type().is_extern_interface_kind()) {
        return;
    }

    AST::ComplexType &owner = type->complex_type();
    const TokenReference at = type->name_token.value();

    size_t ordinal = 0;

    for (AST::FunctionDeclNode *requirement : owner.methods()) {
        if (requirement == nullptr) {
            continue;
        }

        if (!requirement->abi_link_name.has_value()) {
            requirement->abi_link_name = requirement->func_name();
        }

        const std::string slot_name = AST::extern_slot_property_name(ordinal, requirement->func_name());
        const AST::ValueType slot_type = requirement->c_function_type();

        declare_slot(payload, type, slot_name, slot_type, at);
        ++ordinal;
    }

    AST::ValueType image_type = AST::ValueType::make_unknown();

    if (payload.collector.core_types.has(AST::CoreTypeKind::t_erased)) {
        image_type = payload.collector.core_types.type(AST::CoreTypeKind::t_erased);
    }

    declare_slot(payload, type, AST::k_extern_image_property, image_type, at);
}

void Parser::plant_extern_interface_bodies(Payload &payload, AST::TypeDeclNode *type)
{
    if (type == nullptr || !type->complex_type().is_extern_interface_kind()) {
        return;
    }

    const TokenReference at = type->name_token.value();
    size_t ordinal = 0;

    for (AST::FunctionDeclNode *requirement : type->complex_type().methods()) {
        if (requirement == nullptr) {
            continue;
        }

        const std::string slot_name = AST::extern_slot_property_name(ordinal, requirement->func_name());
        plant_forwarder_body(payload, *requirement, slot_name, at);
        ++ordinal;
    }
}
