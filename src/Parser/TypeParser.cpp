#include "Parser/TypeParser.h"
#include "Parser/NamespaceParser.h"
#include "AST/ASTValueType.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"
#include "AST/StructNode.h"

#include <algorithm>
#include <fmt/core.h>


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

bool Parser::starts_qualified_vardecl(Parser::Payload &payload)
{
    size_t offset = 0;
    if (payload.cursor.is_type(Token::Type::t_const)) {
        offset++;
    }

    // walk the `identifier ::` pairs of the namespace path, at least one is required
    size_t pairs = 0;
    while (payload.cursor.is_type_sequence(offset, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        offset += 2;
        pairs++;
    }

    if (pairs == 0) {
        return false;
    }

    // the type name itself followed by the variable name
    return payload.cursor.is_type_sequence(offset, { Token::Type::t_identifier, Token::Type::t_varname });
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

// expands a named constraint alias (e.g. `numeric`) into the set of concrete types it
// covers, or nullopt if `name` is not a known alias. the sets are derived from the existing
// ValueType category predicates so they stay in sync with the type system automatically.
static std::optional<std::vector<AST::ValueType>> expand_type_alias(const std::string &name)
{
    // the primitives an alias may enumerate over (no void/complex)
    static const AST::ValueTypePrimitive all_primitives[] = {
        AST::ValueTypePrimitive::t_int8, AST::ValueTypePrimitive::t_int16,
        AST::ValueTypePrimitive::t_int32, AST::ValueTypePrimitive::t_int64,
        AST::ValueTypePrimitive::t_uint8, AST::ValueTypePrimitive::t_uint16,
        AST::ValueTypePrimitive::t_uint32, AST::ValueTypePrimitive::t_uint64,
        AST::ValueTypePrimitive::t_float32, AST::ValueTypePrimitive::t_float64,
        AST::ValueTypePrimitive::t_bool,
    };

    bool (AST::ValueType::*pred)() const = nullptr;
    if (name == "numeric") {
        pred = &AST::ValueType::is_numeric_type;
    } else if (name == "integer") {
        pred = &AST::ValueType::is_integer_type;
    } else if (name == "signed") {
        pred = &AST::ValueType::is_signed_integer;
    } else if (name == "unsigned") {
        pred = &AST::ValueType::is_unsigned_integer;
    } else if (name == "floating") {
        pred = &AST::ValueType::is_floating_type;
    } else {
        return std::nullopt;
    }

    std::vector<AST::ValueType> out;
    for (auto primitive : all_primitives) {
        AST::ValueType value(primitive);
        if ((value.*pred)()) {
            out.push_back(value);
        }
    }
    return out;
}

// resolves a single constraint atom to the set of concrete types it admits. an atom is an
// alias, a primitive, or a user struct/class name. returns nullopt if it resolves to none.
static std::optional<std::vector<AST::ValueType>> resolve_constraint_atom(Parser::Payload &payload, const std::string &name)
{
    if (auto alias = expand_type_alias(name)) {
        return alias;
    }

    auto primitive = get_primitive_type(name);
    if (primitive.is_primitive()) {
        return std::vector<AST::ValueType>{ primitive };
    }

    auto symbol = payload.collector.namespaces.find_symbol(name, *payload.context.current_namespace);
    if (symbol && symbol->type() == AST::SymbolType::t_struct) {
        auto *decl = symbol->node.unsafe_ptr<AST::StructDeclNode>();
        return std::vector<AST::ValueType>{ decl->value_type() };
    }

    return std::nullopt;
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

    // enforce any type-parameter constraints on the explicit arguments (e.g. `Vec<bool>`
    // where `Vec<T: numeric>`). skip args still mentioning a type parameter - those are
    // resolved and re-checked once the enclosing template is instantiated.
    for (size_t i = 0; i < args.size(); i++) {
        const auto *param = template_ct->type_parameters[i];
        if (param->is_constrained() && !args[i].is_type_param() && !param->allows(args[i])) {
            payload.collector.collect_issue<AST::Issue::UnsatisfiedTypeConstraint>(
                payload.context.code_ref(name_token),
                "Type parameter '" + param->name + "' of '" + template_ct->name.value_or(name_token.value()) +
                "' is constrained to '" + param->constraint_spelling +
                "' but was given '" + args[i].get_type_desciption() + "'"
            );
            return AST::ValueType::make_unknown();
        }
    }

    auto *inst = payload.collector.type_registry.get_or_create_instantiation(template_ct, args);
    return template_decl->value_type().is_class()
        ? AST::ValueType::make_class(inst)
        : AST::ValueType::make_struct(inst);
}

std::vector<Parser::ParsedTypeParam> Parser::parse_type_param_list(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    std::vector<ParsedTypeParam> type_parameters;

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

        ParsedTypeParam param { cursor.current(), {}, "" };

        // a repeated name would silently alias, since name resolution takes the first match and
        // every later same-named parameter becomes unreachable
        for (const auto &seen : type_parameters) {
            if (seen.name() == param.name()) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(param.name_token),
                    fmt::format("Type parameter '{}' is already declared in this list", param.name())
                );
                cursor.try_skip_to_next_statement();
                return type_parameters;
            }
        }

        cursor.skip();

        // optional constraint: `: atom (| atom)*`
        if (cursor.is_type(Token::Type::t_colon)) {
            cursor.skip(); // skip ':'

            while (true) {
                if (!cursor.is_type(Token::Type::t_identifier)) {
                    payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
                    cursor.try_skip_to_next_statement();
                    return type_parameters;
                }

                auto atom_token = cursor.current();
                auto atom_name = atom_token.value();

                auto atom_types = resolve_constraint_atom(payload, atom_name);
                if (!atom_types) {
                    payload.collector.collect_issue<AST::Issue::GenericError>(
                        payload.context.code_ref(atom_token),
                        fmt::format("Unknown type or alias '{}' in constraint of type parameter '{}'", atom_name, param.name())
                    );
                    cursor.try_skip_to_next_statement();
                    return type_parameters;
                }

                for (const auto &type : *atom_types) {
                    param.constraint.push_back(type);
                }
                if (!param.constraint_spelling.empty()) {
                    param.constraint_spelling += "|";
                }
                param.constraint_spelling += atom_name;
                cursor.skip();

                // more atoms are separated by '|'
                if (cursor.is_type(Token::Type::t_or)) {
                    cursor.skip();
                    continue;
                }
                break;
            }
        }

        type_parameters.push_back(std::move(param));

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

// mints (or reuses) the owned declarations for a freshly parsed parameter list.
//
// reuse matters for correctness, not just allocation: a module is parsed twice — a symbol pass
// then a full pass — each with a fresh Context, and both reach this point for the same list.
// minting new declarations the second time would give the two passes distinct parameters, so a
// generic struct's self-application Foo<T> would intern twice and the two Foo<T> would compare
// unequal. reusing whenever the shape is unchanged keeps a single declaration per parameter.
static std::vector<AST::TypeParamDecl *> declare_params(
    Parser::Payload &payload,
    const std::vector<AST::TypeParamDecl *> &existing,
    const std::vector<Parser::ParsedTypeParam> &parsed)
{
    bool reusable = existing.size() == parsed.size();
    for (size_t i = 0; reusable && i < parsed.size(); i++) {
        reusable = existing[i]->name == parsed[i].name();
    }

    std::vector<AST::TypeParamDecl *> result;
    result.reserve(parsed.size());

    for (size_t i = 0; i < parsed.size(); i++) {
        AST::TypeParamDecl *decl = reusable
            ? existing[i]
            : payload.collector.type_params.declare(parsed[i].name(), i, parsed[i].name_token);

        // constraints are refreshed either way: the symbol pass may have parsed the list before
        // the types a constraint atom names were resolvable
        decl->constraint = parsed[i].constraint;
        decl->constraint_spelling = parsed[i].constraint_spelling;
        result.push_back(decl);
    }

    return result;
}

void Parser::declare_type_parameters(Payload &payload, AST::ComplexType &owner, const std::vector<ParsedTypeParam> &parsed)
{
    auto declared = declare_params(payload, owner.type_parameters, parsed);

    owner.type_parameters.clear();
    for (auto *decl : declared) {
        owner.add_type_parameter(decl);
    }
}

void Parser::declare_type_parameters(Payload &payload, AST::FunctionDeclNode &owner, const std::vector<ParsedTypeParam> &parsed)
{
    owner.type_parameters = declare_params(payload, owner.type_parameters, parsed);
    for (auto *decl : owner.type_parameters) {
        decl->set_owner(&owner);
    }
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

    // a type may be namespace qualified: `a::b::Foo`. the prefix is consumed here so the name
    // token below resolves in that namespace instead of the current one. a qualified name is
    // never a primitive or a type parameter, so those lookups are skipped for it
    const AST::Namespace *lookup_namespace = payload.context.current_namespace;
    bool is_qualified = false;
    if (payload.cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        auto *ns_node = parse_namespace(payload);
        assert(ns_node != nullptr && "expected a namespace node");
        lookup_namespace = ns_node->ast_namespace;
        is_qualified = true;
    }

    auto token = payload.cursor.current();
    auto primitive_type = is_qualified ? AST::ValueType::make_unknown() : get_primitive_type(token.value());
    AST::StructDeclNode *user_type_decl = nullptr;

    // if it's not a primitive type, check for type parameters first
    if (!primitive_type.is_primitive() && !primitive_type.is_struct() && !primitive_type.is_class()) {
        // a generic type parameter in scope, e.g. the T of the enclosing `struct Box<T>`
        const AST::TypeParamDecl *type_param = is_qualified ? nullptr : payload.context.find_type_param(token.value());

        if (type_param) {
            primitive_type = AST::ValueType::make_type_param(type_param);
        } else {
            // Check for user-defined types (structs/classes)
            auto struct_symbol = payload.collector.namespaces.find_symbol(token.value(), *lookup_namespace);
            if (struct_symbol && struct_symbol->type() == AST::SymbolType::t_struct) {
                user_type_decl = struct_symbol->node.unsafe_ptr<AST::StructDeclNode>();
                primitive_type = user_type_decl->value_type();
            }

            // an unresolved qualified name can only be a mistake - the namespace lookup creates
            // missing namespaces on demand, so it would silently degrade to an unknown type
            else if (is_qualified) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(token),
                    "Unknown type '" + lookup_namespace->full_name() + ECO_NAMESPACE_SEPARATOR + token.value() + "'"
                );
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