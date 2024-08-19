#include "Parser/TypeParser.h"
#include "AST/ASTValueType.h"
#include "AST/StructNode.h"
#include <algorithm>


bool Parser::can_parse_type(Parser::Payload &payload)
{
    // a type can be preceded by a const keyword
    size_t offset = 0;
    if (payload.cursor.is_type(Token::Type::t_const)) {
        offset++;
    }

    // a type can be an identifier or ptr
    if (
        payload.cursor.peek_is_type(offset, Token::Type::t_identifier) ||
        payload.cursor.peek_is_type(offset, Token::Type::t_ptr)
    ) {
        return true;
    }

    return false;
}

AST::ValueType get_primitive_type(const std::string &types_string)
{
    if (types_string == "int") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int32);
    } else if (types_string == "int8") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int8);
    } else if (types_string == "int16") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int16);
    } else if (types_string == "int32") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int32);
    } else if (types_string == "int64") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int64);
    } else if (types_string == "uint") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint32);
    } else if (types_string == "uint8") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint8);
    } else if (types_string == "uint16") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint16);
    } else if (types_string == "uint32") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint32);
    } else if (types_string == "uint64") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint64);
    } else if (types_string == "float") {
        return AST::ValueType(AST::ValueTypePrimitive::t_float32);
    } else if (types_string == "float32") {
        return AST::ValueType(AST::ValueTypePrimitive::t_float32);
    } else if (types_string == "float64") {
        return AST::ValueType(AST::ValueTypePrimitive::t_float64);
    } else if (types_string == "bool") {
        return AST::ValueType(AST::ValueTypePrimitive::t_bool);
    } else if (types_string == "void") {
        return AST::ValueType(AST::ValueTypePrimitive::t_void);
    }

    return AST::ValueType::make_unknown();
}

// parses `<Arg, Arg, ...>` (cursor positioned at the opening `<`) as generic type arguments
// applied to `template_decl`, and returns the interned application ValueType. Arguments are
// themselves types, so nesting (Foo<Bar<int>>) falls out of the recursion into parse_type.
static AST::ValueType parse_generic_application(Parser::Payload &payload, AST::StructDeclNode *template_decl, const TokenReference &name_token)
{
    auto &cursor = payload.cursor;
    AST::ComplexType *template_ct = template_decl->value_type().get_complex_type();

    cursor.skip(); // skip '<'

    std::vector<AST::ValueType> args;
    while (!cursor.is_generic_close()) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(name_token), Token::Type::t_close_angle, Token::Type::t_unknown);
            return AST::ValueType::make_unknown();
        }

        auto *arg_type_node = parse_type(payload);
        if (!arg_type_node) {
            return AST::ValueType::make_unknown();
        }
        args.push_back(arg_type_node->type);

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        } else if (!cursor.is_generic_close()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_close_angle, cursor.current().type());
            return AST::ValueType::make_unknown();
        }
    }

    cursor.consume_generic_close(); // consume '>' (splitting a '>>' if present)

    if (!template_ct->is_generic() || template_ct->type_parameters.size() != args.size()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            "Wrong number of type arguments for generic type '" + template_ct->name.value_or(name_token.value()) + "'"
        );
        return AST::ValueType::make_unknown();
    }

    auto *inst = payload.collector.type_registry.get_or_create_instantiation(template_ct, args);
    return template_decl->value_type().is_class()
        ? AST::ValueType::make_class(inst)
        : AST::ValueType::make_struct(inst);
}

std::vector<std::string> Parser::parse_type_param_list(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    std::vector<std::string> type_parameters;

    // no list present
    if (!cursor.is_type(Token::Type::t_open_angle)) {
        return type_parameters;
    }

    cursor.skip(); // skip '<'

    while (!cursor.is_type(Token::Type::t_close_angle)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_close_angle, Token::Type::t_unknown);
            return type_parameters;
        }

        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
            cursor.try_skip_to_next_statement();
            return type_parameters;
        }

        type_parameters.push_back(cursor.current().value());
        cursor.skip();

        // comma separator or closing angle
        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        } else if (!cursor.is_type(Token::Type::t_close_angle)) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_close_angle, cursor.current().type());
            cursor.try_skip_to_next_statement();
            return type_parameters;
        }
    }

    cursor.skip(); // skip '>'
    return type_parameters;
}

AST::TypeNode *Parser::parse_type(Parser::Payload &payload)
{
    bool is_const = false;
    bool is_pointer = false;

    if (payload.cursor.is_type(Token::Type::t_const)) {
        is_const = true;
        payload.cursor.skip();
    }

    if (payload.cursor.is_type(Token::Type::t_ptr)) {
        is_pointer = true;
        payload.cursor.skip();

        // ptr have a generics like syntax ptr<T>
        if (!payload.cursor.is_type(Token::Type::t_open_angle)) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(payload.cursor.current()),
                Token::Type::t_open_angle,
                payload.cursor.type()
            );

            return nullptr;
        }

        payload.cursor.skip();
    }

    auto token = payload.cursor.current();
    auto primitive_type = get_primitive_type(token.value());
    AST::StructDeclNode *user_type_decl = nullptr;

    // if it's not a primitive type, check for type parameters first
    if (!primitive_type.is_primitive() && !primitive_type.is_struct() && !primitive_type.is_class()) {
        // Check if this is a type parameter (generic type like T)
        if (payload.context.is_type_parameter(token.value())) {
            // Find the index of this type parameter
            auto it = std::find(payload.context.current_type_parameters.begin(),
                               payload.context.current_type_parameters.end(),
                               token.value());
            if (it != payload.context.current_type_parameters.end()) {
                size_t index = std::distance(payload.context.current_type_parameters.begin(), it);
                primitive_type = AST::ValueType::make_type_param(index);
            }
        } else {
            // Check for user-defined types (structs/classes)
            auto struct_symbol = payload.collector.namespaces.find_symbol(token.value(), *payload.context.current_namespace);
            if (struct_symbol && struct_symbol->type() == AST::SymbolType::t_struct) {
                user_type_decl = struct_symbol->node.unsafe_ptr<AST::StructDeclNode>();
                primitive_type = user_type_decl->value_type();
            }
        }
    }

    payload.cursor.skip();

    // generic application on a user type: `Name<Arg, Arg, ...>` (nested, e.g. Foo<Bar<int>>).
    // only outside the hardcoded ptr<...> path, whose closing `>` we still own below.
    if (!is_pointer && user_type_decl && payload.cursor.is_type(Token::Type::t_open_angle)) {
        primitive_type = parse_generic_application(payload, user_type_decl, token);
    }

    primitive_type.set_const(is_const);
    primitive_type.set_pointer(is_pointer);

    auto &node = payload.context.emplace_node<AST::TypeNode>(primitive_type, token);
    node.is_const = is_const;
    node.is_pointer = is_pointer;

    // on pointer types we need to close the generics
    if (is_pointer) {
        if (!payload.cursor.is_type(Token::Type::t_close_angle)) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(payload.cursor.current()),
                Token::Type::t_close_angle,
                payload.cursor.type()
            );

            return nullptr;
        }

        payload.cursor.skip();
    }

    return &node;
}