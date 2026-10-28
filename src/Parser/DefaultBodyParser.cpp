#include "Parser/DefaultBodyParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/SymbolParser.h"

#include "AST/ASTClone.h"
#include "AST/ASTConstness.h"
#include "AST/ASTContext.h"
#include "AST/ASTTypeParam.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <vector>

namespace
{
    // the requirement's signature on a second node that carries the body; both passes reuse one node and one `Self`
    AST::FunctionDeclNode &declare_default(
        Parser::Payload &payload,
        AST::FunctionDeclNode &requirement,
        AST::ScopeNode &scope,
        const TokenReference &at
    )
    {
        AST::FunctionDeclNode *def = requirement.default_decl;

        if (def == nullptr) {
            def = &payload.context.emplace_node<AST::FunctionDeclNode>(at);
            def->defaulted_requirement = &requirement;
            requirement.default_decl = def;
        }

        // owned by the interface, so it mangles as the interface's and cannot reach the conformer's privates
        def->ast_namespace = requirement.ast_namespace;
        def->visibility = requirement.visibility;
        def->declared_in = requirement.declared_in;
        def->owner_type = requirement.owner_type;
        def->member_kind = AST::MemberKind::t_method;
        def->return_type = requirement.return_type;
        def->where_clauses = requirement.where_clauses;
        def->attributes = requirement.attributes;
        def->doc_comment = requirement.doc_comment;

        const size_t inherited = requirement.inherited_type_param_count;
        const std::vector<AST::TypeParamDecl *> &associated = requirement.owner_type->associated_types();

        // `Self` is constrained by the interface the requirement's receiver names
        const AST::ValueType interface =
            AST::ValueType::make_mutable(requirement.args[0]->type().pointee());

        AST::TypeParamDecl *self = def->default_self_param();

        if (self == nullptr) {
            self = payload.collector.type_params.declare("Self", inherited, at);
            self->set_owner(def);
        }

        self->ordinal = inherited + associated.size();
        self->constraint = { interface };
        self->constraint_spelling = interface.get_type_desciption();

        // [interface parameters..., associated types..., Self, own...]: Self joins the inherited prefix
        const auto split = requirement.type_parameters.begin() + inherited;

        std::vector<AST::TypeParamDecl *> params(requirement.type_parameters.begin(), split);
        params.insert(params.end(), associated.begin(), associated.end());
        params.push_back(self);
        params.insert(params.end(), split, requirement.type_parameters.end());

        def->type_parameters = std::move(params);
        def->inherited_type_param_count = inherited + associated.size() + 1;

        // `Self&`, or `const Self&` for a `const function`
        AST::ValueType receiver = AST::ValueType::make_type_param(self);

        if (AST::receiver_is_const(requirement)) {
            receiver = AST::ValueType::make_const(receiver);
        }

        auto &receiver_node = payload.context.emplace_node<AST::TypeNode>(
            AST::ValueType::make_pointer(receiver, false));

        std::vector<AST::VarDeclNode *> args;
        Parser::push_receiver_param(payload, args, scope, &receiver_node, at);

        // cloned, since the requirement's parameters live in a scope no body is parsed in
        const AST::TypeSubstitution none;
        AST::CloneContext cc(payload.context.module.nodes, none, payload.collector.type_registry);

        for (size_t i = 1; i < requirement.args.size(); i++) {
            AST::VarDeclNode *param = cc.child(requirement.args[i]);

            if (param != nullptr) {
                scope.add_vardecl(*param);
            }

            args.push_back(param);
        }

        // what AST::default_interface_name reads off the receiver
        for (AST::VarDeclNode *arg : args) {
            if (arg != nullptr) {
                arg->declaring_function = def;
            }
        }

        def->replace_args(std::move(args));

        return *def;
    }
}

bool Parser::starts_default_body(const Payload &payload, const AST::FunctionDeclNode &requirement)
{
    return requirement.has_receiver()
        && requirement.is_interface_requirement()
        && requirement.own_type_param_count() == 0
        && !requirement.args.empty()
        && requirement.args[0] != nullptr
        && requirement.args[0]->has_type()
        && payload.cursor.is_type(Token::Type::t_open_brace);
}

bool Parser::parse_default_body(
    Payload &payload,
    AST::FunctionDeclNode &requirement,
    const TokenReference &at,
    bool symbol_only
)
{
    auto &scope = payload.context.emplace_node<AST::ScopeNode>();
    AST::FunctionDeclNode &def = declare_default(payload, requirement, scope, at);

    // the same frame the body pass opens, so a nested `function` lands in one namespace in both passes
    if (symbol_only) {
        AST::FunctionBodyScope body_scope(payload.context, &def);
        Parser::parse_declaration_surface(payload, payload.cursor.current());
        return true;
    }

    // both go to the file root, where later passes and the monomorphizer find them
    payload.context.declaration_scope().add_funcdecl(requirement);
    payload.context.declaration_scope().add_funcdecl(def);

    return Parser::parse_function_body(payload, def, scope);
}
