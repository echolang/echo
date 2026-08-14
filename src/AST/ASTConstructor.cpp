#include "AST/ASTConstructor.h"

#include "AST/ASTControlFlow.h"
#include "AST/ASTModule.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

AST::VarDeclNode &AST::declare_constructor_this(
    AST::Module &module,
    AST::TypeNode &self_type,
    const TokenReference &at
)
{
    auto &decl = module.nodes.emplace_back<AST::VarDeclNode>(
        module.make_virtual_token("$this", Token::Type::t_varname, at), &self_type);

    // **the one place a `$this` is `out`.** the storage arrives holding nothing and the body owes it
    // an initialized value, which is the whole of why a constructor may write every property without
    // first destroying what was there.
    //
    // it goes on the declaration and not on a parameter because this `$this` *is* a local - a
    // constructor has no receiver argument, so AST::access_effect_of has nothing to answer for at
    // args[0] and deliberately does not pretend to
    decl.access_effect = AST::AccessEffect::t_out;

    // the one fork between the two storage classes, and the only thing this function decides. a
    // struct's slot is already there when the frame is entered; a class's handle names a block that
    // has to be made, so the declaration carries the allocation as its initializer
    if (self_type.type.is_class()) {
        decl.init_expr = &module.nodes.emplace_back<AST::ClassAllocExprNode>(self_type.type, at);
    }

    return decl;
}

void AST::close_constructor_body(
    AST::Module &module,
    AST::FunctionDeclNode &decl,
    AST::VarDeclNode &this_decl
)
{
    if (decl.body == nullptr || AST::scope_always_leaves_function(*decl.body)) {
        return;
    }

    auto &var = module.nodes.emplace_back<AST::VarNode>(&this_decl);
    auto &read = module.nodes.emplace_back<AST::VarRefNode>(&var);
    auto &ret = module.nodes.emplace_back<AST::ReturnNode>(&read);

    decl.body->children.push_back(AST::make_ref(ret));
}

AST::ExprNode *AST::make_member_place(
    AST::Module &module,
    AST::VarDeclNode &local,
    const std::string &name,
    const TokenReference &at
)
{
    auto &var = module.nodes.emplace_back<AST::VarNode>(&local, local.token_varname);
    auto &read = module.nodes.emplace_back<AST::VarRefNode>(&var);

    return &module.nodes.emplace_back<AST::MemberAccessNode>(
        AST::make_ref(read),
        module.make_virtual_token(name, Token::Type::t_identifier, at));
}

AST::AssignNode &AST::seat_property_from_parameter(
    AST::Module &module,
    AST::VarDeclNode &self,
    const AST::VarDeclNode &property,
    AST::VarDeclNode *parameter,
    const TokenReference &at
)
{
    AST::ExprNode *target = AST::make_member_place(module, self, property.name(), at);

    if (property.has_type() && property.type().is_pointer()) {
        target = &module.nodes.emplace_back<AST::PointerValueNode>(
            target, module.make_virtual_token(property.name(), Token::Type::t_identifier, at));
    }

    auto &param_var = module.nodes.emplace_back<AST::VarNode>(parameter);
    auto &param_read = module.nodes.emplace_back<AST::VarRefNode>(&param_var);

    auto &write = module.nodes.emplace_back<AST::AssignNode>(target, &param_read, at);

    write.is_initialization = true;
    write.hands_over_value = true;

    return write;
}
