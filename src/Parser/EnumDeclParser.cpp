#include "Parser/EnumDeclParser.h"

#include "AST/ASTConstructor.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTStringLiteral.h"
#include "AST/AssignNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/MatchExprNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/TypeParser.h"

#include <fmt/core.h>

namespace
{
// the widest discriminant a plain enum carries. `uint8` rather than a width chosen from the case
// count, because `__tag` is appended before the body is read - see declare_enum_tag - and because an
// enum wanting more than this has a spelling for it already
constexpr size_t k_max_plain_enum_cases = 256;

// **a slot of an enum's layout, as a declaration** - both of the two the compiler mints, `__tag` and
// one per payload field.
//
// a VarDeclNode rather than a bare ComplexType property, because TypeDeclNode::add_property writes
// both stores and TypeLowering::create_llvm_struct_decl reads the *declaration* list. a property
// appended to the layout alone would be invisible to codegen, which is a struct whose LLVM type has
// fewer fields than its members - a GEP past the end, silently
//
// **not private, and that is the tagged optional's decision read again.** `__has` and `__value` are
// deliberately public for one reason: AST::OwnershipPass and AST::MatchResolution mint reads of them
// into the body of whatever function needed one, where AST::enclosing_type_of answers nothing - so a
// private slot refuses the compiler's own access, against a line nobody wrote.
//
// what the modifier would have bought is bought elsewhere anyway: parse_typedecl refuses an enum a
// field-wise constructor outright rather than relying on the private-property rule, and the names are
// unspellable in the sense that matters - nothing in the grammar produces them, and reading one by
// hand is the same reach into an implementation `$opt->__has` already is
AST::VarDeclNode *declare_enum_property(
    Parser::Payload &payload,
    AST::TypeDeclNode *enum_node,
    const std::string &name,
    const AST::ValueType &type,
    const TokenReference &at
)
{
    // **spelled with the `$`**, because a t_varname token is what a property is declared from and
    // VarDeclNode strips the sigil to get the name. without it the property lands under `_c1_after`
    // and every member access the case constructor mints misses by one character
    //
    // a virtual token because `__c1_after` is a spelling no source holds, positioned at `at` - the
    // field's own token where there is one, so a diagnostic about the slot points at what the author
    // wrote rather than at the enum's name
    auto property_token = payload.context.make_virtual_token(
        "$" + name, Token::Type::t_varname, at);

    // a type node of its own rather than the parameter's: PointerAdjuster and the type checker rewrite
    // edges in place, so one node under two parents is one rewrite per parent
    auto *property_type = payload.context.emplace_nodep<AST::TypeNode>(type);
    auto *property = payload.context.emplace_nodep<AST::VarDeclNode>(property_token, property_type);

    enum_node->add_property(property);

    return property;
}

// the layout half of a case: one slot per field the case carries
AST::VarDeclNode *declare_payload_property(
    Parser::Payload &payload,
    AST::TypeDeclNode *enum_node,
    size_t case_ordinal,
    const AST::VarDeclNode *field
)
{
    return declare_enum_property(
        payload,
        enum_node,
        AST::enum_payload_property_name(case_ordinal, field->name()),
        field->type_node()->type,
        field->token_varname);
}

// **the body of a case constructor, which is a constructor body in every respect but its name.**
//
// so it is built out of AST::declare_constructor_this and AST::close_constructor_body rather than out
// of a local and a `return` written here - those two are documented as the two things every
// constructor body is made of, whoever writes it, and a case is the fourth producer. what they buy is
// the one rule this site would otherwise have had to rediscover: the local is of **value** type, which
// is what makes AST::OwnershipPass move it out on the return instead of copying it and then dropping
// the original at the end of the very function that built it
void build_enum_case_body(
    Parser::Payload &payload,
    AST::TypeDeclNode *enum_node,
    AST::FunctionDeclNode &constructor,
    AST::ScopeNode &body,
    const AST::ComplexType::EnumCase &entry
)
{
    const TokenReference at = constructor.declaration_site_token();

    constructor.body = &body;

    AST::VarDeclNode &value = AST::declare_constructor_this(
        payload.context.module, *constructor.return_type, at);
    body.add_vardecl(value);

    // the discriminant, written first so a reader of `-p ast-resolved` sees the case being chosen
    // before its payload is seated. a `uint8` tag takes a literal typed to it rather than a plain
    // int32 one, or the store is a truncation the type checker would refuse
    auto tag_token = payload.context.make_virtual_token(
        std::to_string(entry.discriminant), Token::Type::t_integer_literal, at);

    const AST::ValueType tag_type = enum_node->complex_type().get_property_type(AST::k_enum_tag_index);

    auto *tag_literal = payload.context.emplace_nodep<AST::LiteralIntExprNode>(
        tag_token, tag_type.get_primitive_type());

    auto *tag_write = payload.context.emplace_nodep<AST::AssignNode>(
        AST::make_member_place(payload.context.module, value, AST::k_enum_tag_name, at),
        tag_literal,
        at);

    // fresh storage: the local was declared one statement ago and holds nothing owed an end
    tag_write->is_initialization = true;
    body.children.push_back(AST::make_ref(*tag_write));

    // and the payload, through the same AST::seat_property_from_parameter the field-wise constructor
    // seats a struct's properties with - the pointer binding, the initialization and the hand-over are
    // one rule, and a case is the fourth body built out of it
    for (size_t i = 0; i < entry.payload_field_count; i++) {
        body.children.push_back(AST::make_ref(
            AST::seat_property_from_parameter(
                payload.context.module,
                value,
                *enum_node->properties()[entry.first_payload_property + i],
                constructor.args[i],
                at)));
    }

    AST::close_constructor_body(payload.context.module, constructor, value);
}
}

std::string Parser::enum_backing_refusal(const AST::ValueType &backing, const AST::CoreTypes &core)
{
    // the closed vocabulary, and the reason it is closed: a discriminant has to be an integer, and
    // `string` is the one other spelling worth having because it is what a name round-trips through.
    // anything else would be storage per value or a second mechanism, and a case is neither
    if (backing.is_integer_type() || core.is_string(backing)) {
        return "";
    }

    return fmt::format(
        "'{}' cannot back an enum - a case's value has to be an integer type or `string`.",
        backing.get_type_desciption());
}

void Parser::declare_enum_tag(Parser::Payload &payload, AST::TypeDeclNode *enum_node)
{
    AST::ComplexType &owner = enum_node->complex_type();

    // the backing type where there is one: an integer-backed enum stores the *written* value, so
    // `case not_found = 404` is a `__tag` of 404 and `->value()` is the discriminant read back. a
    // string backing does not reach here - a string cannot be a discriminant, so those cases number
    // from zero like a plain enum's and the spelling is recovered by the synthesized accessor
    AST::ValueType tag_type = AST::ValueType(AST::ValueTypePrimitive::t_uint8);

    if (owner.enum_backing.has_value() && owner.enum_backing->is_integer_type()) {
        tag_type = owner.enum_backing.value();
    }

    declare_enum_property(
        payload, enum_node, AST::k_enum_tag_name, tag_type, enum_node->name_token.value());
}

void Parser::parse_enum_case(
    Parser::Payload &payload,
    AST::TypeDeclNode *enum_node,
    const AST::ValueType &self_value_type,
    bool collect_members
)
{
    auto &cursor = payload.cursor;
    AST::ComplexType &owner = enum_node->complex_type();

    const TokenReference case_token = cursor.current();
    cursor.skip(); // `case`

    if (!payload.expect_token(Token::Type::t_identifier)) {
        cursor.try_skip_to_next_statement();
        return;
    }

    const TokenReference name_token = cursor.current();
    cursor.skip();

    // **the constructor is built first and the case table is filled from it**, rather than the two
    // being read separately from the same tokens. a case's payload is a parameter list in every
    // respect a signature cares about, so Parser::parse_parameter_list reads it - which is the same
    // decision `constructor(...)` made, and for the same reason: two walks over one grammar drift
    auto &constructor = payload.context.emplace_node<AST::FunctionDeclNode>(name_token);
    constructor.member_kind = AST::MemberKind::t_static_method;
    constructor.owner_type = &owner;
    constructor.ast_namespace = payload.context.current_namespace;
    constructor.is_implicitly_generated = true;

    // a case of a generic enum is a static over that enum, so it carries the owner's parameters
    // exactly as a written `static function` on one does - AST::static_owner_bindings is what binds
    // them at a call site, the case having no receiver to read them off.
    //
    // **through declare_type_parameters rather than by assigning the vector**, which is the difference
    // between inheriting them and merely holding them: that function also sets
    // `inherited_type_param_count`, and without it a case of no arguments had nothing to infer `T`
    // from and nothing saying the owner was allowed to supply it
    Parser::declare_type_parameters(payload, constructor, {}, enum_node->type_parameters());

    auto &return_type = payload.context.emplace_node<AST::TypeNode>(self_value_type);
    constructor.return_type = &return_type;

    auto &constructor_scope = payload.context.emplace_node<AST::ScopeNode>();

    if (cursor.is_type(Token::Type::t_open_paren)) {
        cursor.skip();

        if (!Parser::parse_parameter_list(payload, constructor, constructor_scope, name_token)) {
            return;
        }
    }

    // `case meter = "m";` / `case not_found = 404;` - the backing value, read as a literal rather than
    // as an expression. a constant here would be an expression the constant expander has to reach
    // before the fixpoint, over a declaration that is not in a body; a literal is what the grammar
    // actually needs and what the accessor can fold
    std::optional<TokenReference> value_token;

    if (cursor.is_type(Token::Type::t_assign)) {
        const TokenReference assign_token = cursor.current();
        cursor.skip();

        if (cursor.is_type({ Token::Type::t_integer_literal, Token::Type::t_string_literal })) {
            // emplace rather than assign: a TokenReference holds its collection by reference and so
            // has no copy assignment
            value_token.emplace(cursor.current());
            cursor.skip();
        }
        else {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(assign_token),
                "A case's value has to be an integer or string literal written here.");
            cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_close_brace });
        }
    }

    if (payload.expect_token(Token::Type::t_semicolon)) {
        cursor.skip();
    }

    if (!collect_members) {
        return;
    }

    if (owner.find_enum_case(name_token.value()) != nullptr) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            fmt::format(
                "'{}' already declares a case named '{}'.",
                enum_node->type_name(), name_token.value()));
        return;
    }

    const size_t ordinal = owner.enum_cases().size();

    if (ordinal >= k_max_plain_enum_cases && !owner.enum_backing.has_value()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            fmt::format(
                "'{}' has more than {} cases, which is more than its discriminant can hold. "
                "Write `enum {} : int32` to widen it.",
                enum_node->type_name(), k_max_plain_enum_cases, enum_node->type_name()));
        return;
    }

    AST::ComplexType::EnumCase entry;
    entry.ordinal = ordinal;
    entry.name = name_token.value();
    entry.discriminant = static_cast<int64_t>(ordinal);
    entry.first_payload_property = enum_node->properties().size();
    entry.payload_field_count = constructor.args.size();

    for (const AST::VarDeclNode *field : constructor.args) {
        entry.payload_field_names.push_back(field->name());
        declare_payload_property(payload, enum_node, ordinal, field);
    }

    if (value_token.has_value()) {
        if (value_token->type() == Token::Type::t_string_literal) {
            // **decoded here, where the collector is**, exactly as Parser::parse_operand decodes the
            // literal it builds - a token's value is what was written, quotes and escapes included, and
            // what a case *is* backed by is the bytes. reported here too, a bad escape needing a
            // collector that a ComplexType has no access to
            std::string decoded;

            if (auto error = AST::decode_string_literal(value_token->value(), decoded)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(value_token.value()), error->message);
            }

            entry.backing_string = decoded;
        }
        else {
            entry.discriminant = std::stoll(value_token->value());
        }
    }

    owner.add_enum_case(entry);

    payload.collector.functions.register_static_function(
        payload.collector, payload.context.code_ref(name_token), &constructor, owner);

    // the body is built after the case is recorded, because it writes `__tag` and the payload
    // properties that recording is what created
    build_enum_case_body(payload, enum_node, constructor, constructor_scope, owner.enum_cases().back());
}

void Parser::synthesize_backing_accessor(
    Parser::Payload &payload,
    AST::TypeDeclNode *enum_node,
    const AST::ValueType &self_value_type
)
{
    AST::ComplexType &owner = enum_node->complex_type();

    if (!owner.enum_backing.has_value() || owner.enum_cases().empty()) {
        return;
    }

    const AST::ValueType backing = owner.enum_backing.value();
    const TokenReference at = enum_node->name_token.value();
    auto name_token = payload.context.make_virtual_token("value", Token::Type::t_identifier, at);

    // an ordinary `const function value() : T`, published through the ordinary member registration - so
    // `$unit->value()` resolves, mangles, instantiates and emits with nothing here that codegen or the
    // resolver had to learn. `const` because reading which case a value is holding writes nothing, which
    // is also what lets it be called on a `const Unit&`
    auto &decl = payload.context.emplace_node<AST::FunctionDeclNode>(name_token);
    decl.member_kind = AST::MemberKind::t_method;
    decl.owner_type = &owner;
    decl.ast_namespace = payload.context.current_namespace;
    decl.is_implicitly_generated = true;

    // the owner's parameters, through the one owner of that shape - see parse_enum_case
    Parser::declare_type_parameters(payload, decl, {}, enum_node->type_parameters());

    auto &return_type = payload.context.emplace_node<AST::TypeNode>(backing);
    decl.return_type = &return_type;

    auto &body = payload.context.emplace_node<AST::ScopeNode>();
    decl.body = &body;

    // the receiver, as a **const** borrow - the whole of what `const function` means, per CLAUDE.md
    auto &self_type = payload.context.emplace_node<AST::TypeNode>(
        AST::ValueType::make_pointer(AST::ValueType::make_const(self_value_type), false));

    Parser::push_receiver_param(payload, decl, body, &self_type, at);

    // **the subject is a copy of `$this`**, and it costs nothing: a backed enum has no payload cases -
    // the two are mutually exclusive at the declaration - so its whole value is the discriminant
    auto subject_token = payload.context.make_virtual_token("$__match", Token::Type::t_varname, at);

    auto *this_var = payload.context.emplace_nodep<AST::VarNode>(decl.args[0]);
    auto *this_read = payload.context.emplace_nodep<AST::VarRefNode>(this_var);

    auto &subject_type = payload.context.emplace_node<AST::TypeNode>(AST::ValueType::make_unknown());
    auto &subject = payload.context.emplace_node<AST::VarDeclNode>(subject_token, &subject_type);
    subject.init_expr = this_read;

    auto &match = payload.context.emplace_node<AST::MatchExprNode>(&subject, at);

    for (const AST::ComplexType::EnumCase &entry : owner.enum_cases()) {
        AST::MatchExprNode::Arm arm { at };
        arm.case_name = entry.name;

        // **decided here rather than left to AST::MatchResolution**, which is the difference between a
        // match somebody wrote and this one: the case table is what both this loop and that pass read,
        // and here it is being walked. so the arm knows its own ordinal, and the pass has nothing to
        // work out - it will find `patterns_decided` already true and leave the node alone
        arm.case_ordinal = entry.ordinal;
        arm.scope = &payload.context.emplace_node<AST::ScopeNode>();

        // the value the author wrote after the `=`, rebuilt as the literal it was. an integer backing
        // hands back the discriminant, which for it *is* the written value - so both spellings come out
        // of the case table and neither is stored twice
        if (entry.backing_string.has_value()) {
            auto value_token = payload.context.make_virtual_token(
                entry.backing_string.value(), Token::Type::t_string_literal, at);

            auto *literal = payload.context.emplace_nodep<AST::LiteralStringExprNode>(value_token);

            // **the two things a string literal is not born with**, and AST::InterpolationLowering mints
            // one the same way for the same reason. the bytes are already decoded - they came off the
            // case's own token, quotes and escapes gone - and `core_string_type` is what makes
            // `result_type()` answer `string` rather than the `ptr<const uint8>` fallback it keeps for a
            // program with no standard library. missing, that fallback is a phi of two different LLVM
            // types, which is an assertion inside LLVM rather than a diagnostic
            literal->decoded_value = entry.backing_string.value();

            if (payload.collector.core_types.has(AST::CoreTypeKind::t_string)) {
                literal->core_string_type = payload.collector.core_types.string_type();
            }

            arm.value = literal;
        }
        else {
            auto value_token = payload.context.make_virtual_token(
                std::to_string(entry.discriminant), Token::Type::t_integer_literal, at);

            arm.value = payload.context.emplace_nodep<AST::LiteralIntExprNode>(
                value_token, backing.get_primitive_type());
        }

        match.arms.push_back(arm);
    }

    match.result = backing;
    match.patterns_decided = true;

    body.children.push_back(AST::make_ref(
        payload.context.emplace_node<AST::ReturnNode>(&match, at)));

    payload.collector.functions.register_member_function(
        payload.collector, payload.context.code_ref(name_token), &decl, owner);
}
