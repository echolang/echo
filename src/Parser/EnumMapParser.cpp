#include "Parser/EnumMapParser.h"

#include "AST/ASTClone.h"
#include "AST/ASTDeclarationOrigin.h"
#include "AST/ASTEnumMap.h"
#include "AST/ASTEnumMapType.h"
#include "AST/ASTFile.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNullability.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MatchExprNode.h"
#include "AST/NullNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "Parser/EnumDeclParser.h"
#include "Parser/ExprParser.h"
#include "Parser/TypeParser.h"

#include <fmt/core.h>

#include <memory>
#include <vector>

namespace
{
bool has_from_with_label(AST::ComplexType &owner, const std::string &label)
{
    for (AST::FunctionDeclNode *fn : AST::find_static_functions(&owner, "from")) {
        if (fn != nullptr
            && fn->args.size() == 1
            && fn->args[0] != nullptr
            && fn->args[0]->has_label()
            && fn->args[0]->label() == label) {
            return true;
        }
    }

    return false;
}

void stamp_map_function(AST::FunctionDeclNode &decl, const AST::EnumMap &map)
{
    decl.declared_in = map.declared_in;
    decl.visibility = map.visibility;
    if (map.declared_namespace != nullptr) {
        decl.ast_namespace = map.declared_namespace;
    }
}

void synthesize_map_forward(
    Parser::Payload &payload,
    AST::ComplexType &owner,
    const AST::ValueType &self_value_type,
    AST::EnumMap &map
)
{
    const TokenReference at = map.name_span.is_valid()
        ? map.name_span.first()
        : map.map_span.first();

    if (!AST::find_member_functions(&owner, map.name).empty()) {
        return;
    }

    auto name_token = payload.context.make_virtual_token(map.name, Token::Type::t_identifier, at);

    const AST::ValueType returned = map.total
        ? map.value_type
        : payload.collector.type_registry.get_or_create_optional(map.value_type);

    auto &decl = Parser::begin_synthesized_enum_function(
        payload, owner, name_token, AST::MemberKind::t_method, returned);

    Parser::push_enum_const_receiver(payload, decl, self_value_type, at);

    auto subject_token = payload.context.make_virtual_token("$__match", Token::Type::t_varname, at);

    auto *this_var = payload.context.emplace_nodep<AST::VarNode>(decl.args[0]);
    auto *this_read = payload.context.emplace_nodep<AST::VarRefNode>(this_var);

    auto &subject_type = payload.context.emplace_node<AST::TypeNode>(AST::ValueType::make_unknown());
    auto &subject = payload.context.emplace_node<AST::VarDeclNode>(subject_token, &subject_type);
    subject.init_expr = this_read;

    auto &match = payload.context.emplace_node<AST::MatchExprNode>(&subject, at);

    AST::TypeSubstitution subst;
    AST::CloneContext cc(
        payload.context.module.nodes, subst, payload.collector.type_registry);

    for (AST::EnumMap::Association &assoc : map.associations) {
        AST::MatchExprNode::Arm arm { at };
        arm.case_name = assoc.case_name;
        arm.case_ordinal = assoc.case_ordinal;
        arm.scope = &payload.context.emplace_node<AST::ScopeNode>();
        AST::ExprNode *rhs = cc.child(assoc.value);
        arm.value = map.total
            ? rhs
            : payload.context.emplace_nodep<AST::TypeCastNode>(returned, rhs, true);
        match.arms.push_back(arm);
    }

    if (!map.total) {
        AST::MatchExprNode::Arm else_arm { at };
        else_arm.scope = &payload.context.emplace_node<AST::ScopeNode>();
        auto *null_node = payload.context.emplace_nodep<AST::NullNode>(
            payload.context.make_virtual_token("null", Token::Type::t_null, at));
        AST::bind_null_to(null_node, returned);
        else_arm.value = null_node;
        match.arms.push_back(else_arm);
    }

    match.result = returned;
    match.patterns_decided = true;

    decl.body->children.push_back(AST::make_ref(
        payload.context.emplace_node<AST::ReturnNode>(&match, at)));

    payload.collector.functions.register_member_function(
        payload.collector, payload.context.code_ref(name_token), &decl, owner);

    stamp_map_function(decl, map);
    map.forward = &decl;
}

void synthesize_map_reverse(
    Parser::Payload &payload,
    AST::ComplexType &owner,
    const AST::ValueType &self_value_type,
    AST::EnumMap &map
)
{
    const TokenReference at = map.name_span.is_valid()
        ? map.name_span.first()
        : map.map_span.first();

    if (has_from_with_label(owner, map.name)) {
        return;
    }

    auto name_token = payload.context.make_virtual_token("from", Token::Type::t_identifier, at);

    const AST::ValueType optional = payload.collector.type_registry.get_or_create_optional(self_value_type);

    auto &decl = Parser::begin_synthesized_enum_function(
        payload, owner, name_token, AST::MemberKind::t_static_method, optional);

    Parser::push_enum_raw_param(payload, decl, map.value_type, at);
    if (!decl.args.empty() && decl.args[0] != nullptr) {
        decl.args[0]->label_token.emplace(at);
    }

    AST::TypeSubstitution subst;
    AST::CloneContext cc(
        payload.context.module.nodes, subst, payload.collector.type_registry);

    for (const AST::EnumMap::Association &assoc : map.associations) {
        auto constructors = AST::find_static_functions(&owner, assoc.case_name);
        if (constructors.empty() || assoc.value == nullptr) {
            continue;
        }

        AST::ExprNode *rhs = cc.child(assoc.value);
        Parser::plant_enum_from_equality_arm(
            payload, decl, constructors[0], self_value_type, optional, rhs, at);
    }

    Parser::plant_enum_from_null_return(payload, decl, optional, at);

    payload.collector.functions.register_static_function(
        payload.collector, payload.context.code_ref(name_token), &decl, owner);

    stamp_map_function(decl, map);
    map.reverse = &decl;
}

void mint_one_map(Parser::Payload &payload, AST::EnumMap &map)
{
    if (map.owner == nullptr) {
        return;
    }

    AST::ComplexType &owner = *map.owner;
    const AST::ValueType self_value_type = AST::ValueType::make_complex(&owner);
    const std::string owner_name = owner.namespaced_name();

    const TokenReference at = map.map_span.is_valid()
        ? map.map_span.first()
        : map.name_span.first();

    if (owner.is_generic()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            fmt::format(
                "A map cannot be declared on the generic enum '{}' - write it on a concrete enum.",
                owner_name));
        return;
    }

    if (owner.is_open_enum()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            fmt::format(
                "A map cannot be declared on the open enum '{}' - the leftover is not a case "
                "the reverse could name.",
                owner_name));
        return;
    }

    if (owner.has_payload_case()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            fmt::format(
                "A map cannot be declared on '{}' - a map is a relation over payload-free cases.",
                owner_name));
        return;
    }

    std::vector<bool> seen(owner.enum_cases().size(), false);
    bool associations_ok = true;

    for (AST::EnumMap::Association &assoc : map.associations) {
        const TokenReference case_at = assoc.case_span.is_valid()
            ? assoc.case_span.first()
            : at;

        const AST::ComplexType::EnumCase *entry = owner.find_enum_case(assoc.case_name);
        if (entry == nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(case_at),
                fmt::format(
                    "'{}' has no case named '{}'.",
                    owner_name, assoc.case_name));
            associations_ok = false;
            continue;
        }

        if (entry->ordinal < seen.size() && seen[entry->ordinal]) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(case_at),
                fmt::format(
                    "'.{}' already appears in '{}' - every case appears once.",
                    assoc.case_name, map.name));
            associations_ok = false;
            continue;
        }

        if (entry->ordinal < seen.size()) {
            seen[entry->ordinal] = true;
        }

        assoc.case_ordinal = entry->ordinal;
    }

    if (!associations_ok) {
        return;
    }

    map.total = true;
    for (const AST::ComplexType::EnumCase &entry : owner.enum_cases()) {
        if (entry.ordinal >= seen.size() || !seen[entry.ordinal]) {
            map.total = false;
            break;
        }
    }

    const std::string range_refusal = AST::enum_map_range_refusal(map.value_type);
    if (!range_refusal.empty()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at), range_refusal);
        return;
    }

    synthesize_map_forward(payload, owner, self_value_type, map);
    synthesize_map_reverse(payload, owner, self_value_type, map);
    map.minted = true;
}

void plant_map_functions(Parser::Payload &payload, AST::EnumMap &map)
{
    if (map.forward != nullptr) {
        payload.context.declaration_scope().add_funcdecl(*map.forward);
    }
    if (map.reverse != nullptr) {
        payload.context.declaration_scope().add_funcdecl(*map.reverse);
    }
}
}

void Parser::parse_file_scope_enum_map(
    Payload &payload,
    const std::optional<TokenReference> &block_token,
    const VisibilityPrefix &visibility
)
{
    if (block_token.has_value()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(payload.cursor.current()),
            "A map belongs at file or namespace scope, or inside the enum it maps.");
        parse_enum_map(payload, nullptr, false, visibility);
        return;
    }

    parse_enum_map(
        payload, nullptr, payload.pass == Pass::t_declarations, visibility);
}

void Parser::parse_enum_map(
    Payload &payload,
    AST::TypeDeclNode *enum_node,
    bool collect_members,
    const VisibilityPrefix &visibility
)
{
    auto &cursor = payload.cursor;
    const TokenReference map_token = cursor.current();
    cursor.skip(); // `map`

    if (!payload.expect_token(Token::Type::t_identifier)) {
        return;
    }

    const TokenReference name_token = cursor.current();
    cursor.skip();

    if (!payload.expect_token(Token::Type::t_colon)) {
        return;
    }
    cursor.skip();

    AST::TypeNode *type_node = Parser::parse_type(payload);
    if (type_node == nullptr) {
        cursor.skip_until({ Token::Type::t_open_brace, Token::Type::t_close_brace, Token::Type::t_semicolon, Token::Type::t_for });
        if (cursor.is_type(Token::Type::t_for)) {
            cursor.skip();
            Parser::parse_type(payload);
        }
        if (cursor.is_type(Token::Type::t_open_brace)) {
            cursor.skip();
            cursor.skip_till_end_of_scope();
        }
        return;
    }

    AST::TypeNode *target_type = nullptr;
    bool saw_for = false;

    if (cursor.is_type(Token::Type::t_for)) {
        saw_for = true;
        const TokenReference for_token = cursor.current();
        cursor.skip();

        if (enum_node != nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(for_token),
                "A map inside the enum already names it - `for` belongs on a map written outside.");
        }

        target_type = Parser::parse_type(payload);
    }
    else if (enum_node == nullptr) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(map_token),
            "A map at file scope has to name the enum: `map glfw : int32 for KeyCode { ... }`.");
    }

    if (!payload.expect_token(Token::Type::t_open_brace)) {
        return;
    }
    cursor.skip();

    struct ParsedAssoc
    {
        TokenReference case_token;
        AST::ExprNode *value = nullptr;
        TokenSpan value_span;
    };

    std::vector<ParsedAssoc> parsed;

    while (!cursor.is_done() && !cursor.is_type(Token::Type::t_close_brace)) {
        if (!cursor.is_type(Token::Type::t_dot)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cursor.current()),
                "A map association is `.case = <value>;`.");
            cursor.try_skip_to_next_statement({ Token::Type::t_open_brace });
            if (cursor.is_type(Token::Type::t_open_brace)) {
                cursor.skip();
                cursor.skip_till_end_of_scope();
            }
            continue;
        }

        cursor.skip(); // `.`

        if (!payload.expect_token(Token::Type::t_identifier)) {
            continue;
        }

        const TokenReference case_token = cursor.current();
        cursor.skip();

        if (!payload.expect_token(Token::Type::t_assign)) {
            continue;
        }
        cursor.skip();

        const auto value_start = cursor.snapshot();
        AST::ExprNode *value = Parser::parse_expr(payload, type_node);
        const auto value_end = cursor.snapshot();

        if (payload.expect_token(Token::Type::t_semicolon)) {
            cursor.skip();
        }

        parsed.push_back(ParsedAssoc {
            case_token,
            value,
            TokenSpan::upto(cursor.tokens, value_start.index, value_end.index),
        });
    }

    if (payload.expect_token(Token::Type::t_close_brace)) {
        cursor.skip();
    }

    if (!collect_members) {
        return;
    }

    AST::ComplexType *owner = nullptr;
    std::string owner_name;

    if (enum_node != nullptr) {
        if (saw_for) {
            return;
        }

        owner = &enum_node->complex_type();
        owner_name = enum_node->type_name();
    }
    else if (target_type != nullptr) {
        if (!target_type->type.is_enum()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(map_token),
                fmt::format(
                    "A map has to name an enum - '{}' is not one.",
                    target_type->type.get_type_desciption()));
            return;
        }

        owner = target_type->type.get_complex_type();
        owner_name = owner != nullptr ? owner->namespaced_name() : target_type->type.get_type_desciption();
    }

    if (owner == nullptr) {
        return;
    }

    if (owner->find_enum_map(name_token.value()) != nullptr) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            fmt::format(
                "'{}' already declares a map named '{}'.",
                owner_name, name_token.value()));
        return;
    }

    const std::string kind_refusal = AST::enum_map_range_kind_refusal(
        name_token.value(), type_node->type);
    if (!kind_refusal.empty()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(map_token), kind_refusal);
        return;
    }

    AST::EnumMap map;
    map.name = name_token.value();
    map.name_span = TokenSpan::of(name_token);
    map.map_span = TokenSpan::of(map_token);
    map.value_type = type_node->type;
    map.declared_in = AST::origin_at(payload.context);
    map.declared_namespace = payload.context.current_namespace;
    map.visibility = enum_node != nullptr
        ? AST::Visibility::t_public
        : visibility.value;

    for (const ParsedAssoc &entry : parsed) {
        AST::EnumMap::Association assoc;
        assoc.case_name = entry.case_token.value();
        assoc.case_span = TokenSpan::of(entry.case_token);
        assoc.value_span = entry.value_span;
        assoc.value = entry.value;
        map.associations.push_back(std::move(assoc));
    }

    map.owner = owner;

    if (payload.context.file.file == nullptr) {
        return;
    }

    auto stored = std::make_unique<AST::EnumMap>(std::move(map));
    owner->add_enum_map(stored.get());
    payload.context.file.file->enum_maps.push_back(std::move(stored));
}

void Parser::mint_file_enum_maps(Payload &payload)
{
    const AST::File *file = payload.context.file.file;
    if (file == nullptr) {
        return;
    }

    for (auto &map : file->enum_maps) {
        if (map != nullptr) {
            mint_one_map(payload, *map);
        }
    }
}

void Parser::plant_file_enum_maps(Payload &payload)
{
    const AST::File *file = payload.context.file.file;
    if (file == nullptr) {
        return;
    }

    for (auto &map : file->enum_maps) {
        if (map != nullptr) {
            plant_map_functions(payload, *map);
        }
    }
}
