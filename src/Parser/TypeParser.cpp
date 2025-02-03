#include "Parser/TypeParser.h"
#include "Parser/NamespaceParser.h"
#include "AST/ASTValueType.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

#include <algorithm>
#include <optional>
#include <fmt/core.h>

// the type grammar is mutually recursive: a generic argument is a type, and a type may be a
// generic application. both work on bare ValueTypes, only the public entry point makes a node
static std::optional<AST::ValueType> parse_value_type(Parser::Payload &payload);

bool Parser::starts_callable_type(Parser::Cursor &cursor, size_t offset)
{
    // the `<` is what tells it from a *declaration*, which has an identifier there, and from a closure
    // literal, which has a `(`. one token of lookahead, and the three spellings cannot overlap
    return cursor.peek_is_type(offset, Token::Type::t_function) &&
        cursor.peek_is_type(offset + 1, Token::Type::t_open_angle);
}

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

    return starts_callable_type(payload.cursor, offset);
}

// advances past one type if the tokens at the cursor form one, and answers whether they did
//
// shape only - no symbol lookup, no diagnostics, no nodes - because this runs at a statement head,
// where the answer decides which parser is called at all. a type name is not necessarily resolvable
// there either: the type-name pass may not have reached it, and an unresolved unqualified name is
// silently `unknown` rather than a diagnostic
//
// the grammar it walks mirrors parse_value_type:
//   type      := 'const'? ( 'ptr' '<' type '>' | qualified ) ref?
//   qualified := ( identifier '::' )* identifier ( '<' type ( ',' type )* '>' )?
//   ref       := '&'
// it consumes its closing angle brackets through the cursor's own '>>' split rather than counting
// them, so `Box<Box<int32>>` cannot end in a different place here than it does in
// parse_generic_application - two notions of "where does this argument list end" would desync
//
// only the bounds-safe cursor accessors are used (never current(), which asserts), so a truncated
// type runs out of tokens and answers false instead of aborting
static bool skip_type_shape(Parser::Cursor &cursor);

// skips a comma-separated list of type shapes, up to but not through whatever closes it. the grammar
// has two such lists - a callable's parameters and a generic argument list - and they differ in
// nothing but `is_end`, so the trailing-comma rule is decided here once for both
template <typename IsEnd>
static bool skip_type_list(Parser::Cursor &cursor, IsEnd is_end)
{
    while (!is_end(cursor)) {
        if (cursor.is_done() || !skip_type_shape(cursor)) {
            return false;
        }

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        } else if (!is_end(cursor)) {
            return false;
        }
    }

    return true;
}

static bool skip_type_shape(Parser::Cursor &cursor)
{
    if (cursor.is_type(Token::Type::t_const)) {
        cursor.skip();
    }

    // `function<R(P...)>`, walked in the same shape-only way: the return type, then a parenthesised
    // parameter list. the angle brackets close through the cursor's `>>` split like every other
    // argument list, so `function<void(ptr<Box<int32>>)>` ends in one place here and in
    // parse_value_type
    if (Parser::starts_callable_type(cursor)) {
        cursor.skip(2);

        if (!skip_type_shape(cursor)) {
            return false;
        }

        if (!cursor.is_type(Token::Type::t_open_paren)) {
            return false;
        }
        cursor.skip();

        if (!skip_type_list(cursor, [](Parser::Cursor &c) { return c.is_type(Token::Type::t_close_paren); })) {
            return false;
        }

        cursor.skip(); // the close paren

        if (!cursor.is_generic_close()) {
            return false;
        }
        cursor.consume_generic_close();
    }
    // `ptr<T>` is a type constructor, so it recurses on its pointee exactly as the parser does
    else if (cursor.is_type(Token::Type::t_ptr)) {
        cursor.skip();

        if (!cursor.is_type(Token::Type::t_open_angle)) {
            return false;
        }
        cursor.skip();

        if (!skip_type_shape(cursor) || !cursor.is_generic_close()) {
            return false;
        }
        cursor.consume_generic_close();
    }
    else {
        // an optionally qualified name, `a::b::Foo`
        while (cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
            cursor.skip(2);
        }

        if (!cursor.is_type(Token::Type::t_identifier)) {
            return false;
        }
        cursor.skip();

        // a generic application, `Foo<Arg, Arg>`. a bare identifier is never a value operand -
        // values carry a `$` - so a `<` after a type name is a type argument list and nothing else,
        // which is why this needs no reinterpretation the way `->name<` does
        if (cursor.is_type(Token::Type::t_open_angle)) {
            cursor.skip();

            if (!skip_type_list(cursor, [](Parser::Cursor &c) { return c.is_generic_close(); })) {
                return false;
            }

            cursor.consume_generic_close();
        }
    }

    // the borrow suffix, in either of the two spellings the lexer produces (see parse_ref_suffix)
    if (cursor.is_type(Token::Type::t_ref) || cursor.is_type(Token::Type::t_and)) {
        cursor.skip();
    }

    return true;
}

bool Parser::starts_vardecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    // `const` and `ptr` begin nothing else at a statement head, so they answer without a scan.
    // that keeps a malformed one reported by parse_varexpr, which knows what it was reading,
    // rather than by the statement dispatch's catch-all
    if (cursor.is_type(Token::Type::t_const) || cursor.is_type(Token::Type::t_ptr)) {
        return true;
    }

    // the inferred form, `$x = ...`, which has no type to scan
    if (cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_assign })) {
        return true;
    }

    // everything else is a declaration exactly when a whole type is followed by a variable name.
    // one scan rather than a list of token sequences: the list had an arm per spelling and no arm
    // for a generic application at all, so `Q<int32> $q` fell through to the call statement branch
    // and was read as a constructor call whose `(` never arrives - and the struct member loop asks
    // this same question, which is why a property failed identically
    //
    // the scan moves the cursor and puts it back. the snapshot carries the '>>' split state, so a
    // rolled-back `Box<Box<int32>>` leaves nothing behind for the real parse to trip on
    const auto snapshot = cursor.snapshot();
    const bool is_decl = skip_type_shape(cursor) && cursor.is_type(Token::Type::t_varname);
    cursor.restore(snapshot);

    return is_decl;
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
    } else if (types_string == "usize") {
        return AST::ValueType(AST::ValueTypePrimitive::t_usize);
    } else if (types_string == "isize") {
        return AST::ValueType(AST::ValueTypePrimitive::t_isize);
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
// ValueType category predicates so they stay in sync with the type system automatically
static std::optional<std::vector<AST::ValueType>> expand_type_alias(const std::string &name)
{
    // the primitives an alias may enumerate over (no void/complex)
    static const AST::ValueTypePrimitive all_primitives[] = {
        AST::ValueTypePrimitive::t_int8, AST::ValueTypePrimitive::t_int16,
        AST::ValueTypePrimitive::t_int32, AST::ValueTypePrimitive::t_int64,
        AST::ValueTypePrimitive::t_uint8, AST::ValueTypePrimitive::t_uint16,
        AST::ValueTypePrimitive::t_uint32, AST::ValueTypePrimitive::t_uint64,
        AST::ValueTypePrimitive::t_usize, AST::ValueTypePrimitive::t_isize,
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
// alias, a primitive, or a user struct/class name. returns nullopt if it resolves to none
static std::optional<std::vector<AST::ValueType>> resolve_constraint_atom(Parser::Payload &payload, const std::string &name)
{
    if (auto alias = expand_type_alias(name)) {
        return alias;
    }

    auto primitive = get_primitive_type(name);
    if (primitive.is_primitive()) {
        return std::vector<AST::ValueType>{ primitive };
    }

    // the declaring namespace, not the current one: a type is not block-scoped, so a type name written
    // inside a `{ }` block resolves exactly as it does outside one
    auto symbol = payload.collector.namespaces.find_symbol(name, *payload.context.declaring_namespace());
    if (symbol && symbol->type() == AST::SymbolType::t_type) {
        auto *decl = symbol->node.unsafe_ptr<AST::TypeDeclNode>();
        return std::vector<AST::ValueType>{ decl->value_type() };
    }

    return std::nullopt;
}

// `Owner::Nested` in type position. a nested type lives on its owner rather than in a namespace, so
// the `A::B` prefix has to be tried as a type as well as a namespace - this is the type half.
//
// it commits to consuming only once the leading identifier is *known* to name a type, so an ordinary
// `a::b::Foo` walk reaches the namespace path below with the cursor untouched. the loop is what makes
// `A::B::C` nest twice.
//
// only an unqualified owner is recognised: `text::string::view` would need the namespace resolved
// first, and Parser::parse_namespace consumes `identifier ::` runs greedily. that is a real hole and
// not one this needs - a nested type is reached through its owner, and an owner is a plain type name
// wherever it is in scope
static AST::TypeDeclNode *try_parse_member_type_chain(Parser::Payload &payload, std::optional<TokenReference> &out_name)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        return nullptr;
    }

    // the declaring namespace, not the current one - a type is not block-scoped, the same rule
    // resolve_constraint_atom above states
    auto *symbol = payload.collector.namespaces.find_symbol(
        cursor.current().value(), *payload.context.declaring_namespace());

    if (symbol == nullptr || symbol->type() != AST::SymbolType::t_type) {
        return nullptr;
    }

    AST::TypeDeclNode *owner = symbol->node.unsafe_ptr<AST::TypeDeclNode>();

    // snapshot/restore, the idiom parse_member_call and starts_vardecl already use for exactly this
    // ambiguity: a name can be both a type and a namespace, and every failure below leaves the caller
    // to try the namespace-qualified path. consuming irreversibly here left the cursor mid-name and
    // turned one mis-resolved `A::B` into a cascade of unrelated diagnostics
    const auto start = cursor.snapshot();

    cursor.skip(); // the owner name
    cursor.skip(); // the `::`

    while (true) {
        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collect_unexpected_token(Token::Type::t_identifier);
            cursor.restore(start);
            return nullptr;
        }

        auto name_token = cursor.current();

        // the *local* table: an owner reached by name is a declaration, never an instantiation, so
        // there is nothing for AST::find_member_type's redirect to do here
        AST::TypeDeclNode *nested = owner->complex_type().find_member_type_decl(name_token.value());

        if (nested == nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(name_token),
                fmt::format("'{}' has no nested type '{}'.",
                    owner->namespaced_type_name(), name_token.value()));

            cursor.restore(start);
            return nullptr;
        }

        owner = nested;
        out_name.emplace(name_token); // TokenReference has no copy assignment
        cursor.skip();

        // another `::` continues the chain
        if (!cursor.is_type_sequence(0, { Token::Type::t_namespace_sep, Token::Type::t_identifier })) {
            return owner;
        }

        cursor.skip(); // the `::`
    }
}

// parses `<Arg, Arg, ...>` (cursor positioned at the opening `<`) as generic type arguments
// applied to `template_decl`, and returns the interned application ValueType. Arguments are
// themselves types, so nesting (Foo<Bar<int>>) falls out of the recursion into parse_type
static AST::ValueType parse_generic_application(Parser::Payload &payload, AST::TypeDeclNode *template_decl, const TokenReference &name_token)
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

        auto arg_type = parse_value_type(payload);
        if (!arg_type.has_value()) {
            return AST::ValueType::make_unknown();
        }
        args.push_back(arg_type.value());

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        } else if (!cursor.is_generic_close()) {
            payload.collect_unexpected_token(Token::Type::t_close_angle);
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

    // enforce any type-parameter constraints on the explicit arguments (e.g. `Vec<bool>` where
    // `Vec<T: numeric>`), by the same rule that judges a generic *call*'s inferred arguments - only
    // the message differs, because this one names a type rather than a function
    if (const auto violation = AST::first_constraint_violation(template_ct->type_parameters, args)) {
        const auto *param = template_ct->type_parameters[*violation];

        payload.collector.collect_issue<AST::Issue::UnsatisfiedTypeConstraint>(
            payload.context.code_ref(name_token),
            "Type parameter '" + param->name + "' of '" + template_ct->name.value_or(name_token.value()) +
            "' is constrained to '" + param->constraint_spelling +
            "' but was given '" + args[*violation].get_type_desciption() + "'"
        );
        return AST::ValueType::make_unknown();
    }

    // the instance carries the template's kind, so make_complex answers `Box<int32>` the same way it
    // answers `Box` - no need to ask the declaration which it was
    auto *inst = payload.collector.type_registry.get_or_create_instantiation(template_ct, args);
    return AST::ValueType::make_complex(inst);
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
            payload.collect_unexpected_token(Token::Type::t_identifier);
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

            // the type-name pass reads this list for the one thing it owns - a generic type's arity
            // and its parameter names - and a constraint atom may name a type no pass has registered
            // yet, so resolving one here would report an unknown type for a well formed program. the
            // atoms are still walked, to leave the cursor after the list, and the declaration pass
            // fills the constraint in: declare_params refreshes it on every pass precisely so this
            // can be deferred
            const bool resolve_atoms = payload.pass != Pass::t_type_names;

            while (true) {
                if (!cursor.is_type(Token::Type::t_identifier)) {
                    payload.collect_unexpected_token(Token::Type::t_identifier);
                    cursor.try_skip_to_next_statement();
                    return type_parameters;
                }

                auto atom_token = cursor.current();
                auto atom_name = atom_token.value();

                if (resolve_atoms) {
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
                }

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
            payload.collect_unexpected_token(Token::Type::t_close_angle);
            cursor.try_skip_to_next_statement();
            return type_parameters;
        }
    }

    cursor.skip(); // skip '>'
    return type_parameters;
}

// mints (or reuses) the owned declarations for a freshly parsed parameter list
//
// reuse matters for correctness, not just allocation: a module is parsed twice — a symbol pass
// then a full pass — each with a fresh Context, and both reach this point for the same list
// minting new declarations the second time would give the two passes distinct parameters, so a
// generic struct's self-application Foo<T> would intern twice and the two Foo<T> would compare
// unequal. reusing whenever the shape is unchanged keeps a single declaration per parameter
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

void Parser::declare_type_parameters(
    Payload &payload,
    AST::FunctionDeclNode &owner,
    const std::vector<ParsedTypeParam> &parsed,
    const std::vector<AST::TypeParamDecl *> &inherited)
{
    // the function's *own* parameters, with any inherited prefix taken off first. it has to come off:
    // declare_params decides whether it can reuse the existing declarations by comparing list
    // *sizes*, and the second parse pass reaches this node with the prefix already in place - left
    // there the sizes would mismatch and the own parameters would be re-minted, giving the two
    // passes distinct declarations, which is exactly what the reuse rule exists to prevent
    std::vector<AST::TypeParamDecl *> own(
        owner.type_parameters.begin() + owner.inherited_type_param_count,
        owner.type_parameters.end());

    own = declare_params(payload, own, parsed);
    for (auto *decl : own) {
        decl->set_owner(&owner);
    }

    // a method carries [owner params..., own params...] in one list, so that one TypeSubstitution
    // binds both: the owner's T from the receiver argument, its own U from the rest. the inherited
    // declarations are *shared* rather than re-declared - the same sharing a constructor does -
    // because a TypeParamDecl has exactly one owner, and re-owning the struct's T would trip
    // set_owner's single-owner assert
    owner.type_parameters = inherited;
    owner.type_parameters.insert(owner.type_parameters.end(), own.begin(), own.end());
    owner.inherited_type_param_count = inherited.size();
}

// consumes an optional trailing `&`, turning `T` into the non-nullable borrow `T&`
//
// both t_ref and t_and are accepted. the lexer only emits t_ref when the `&` is immediately
// followed by a name character (src/Lexer.cpp, LexerFunction::ReferenceFrom), so `int32 & $x`
// arrives as t_and and used to lose its reference silently. in type position a following `&`
// can never be a binary operator, so accepting both is unambiguous and needs no lexer change -
// which matters, because the binary/reference distinction elsewhere depends on that rule
static AST::ValueType parse_ref_suffix(Parser::Payload &payload, AST::ValueType type)
{
    if (!payload.cursor.is_type(Token::Type::t_ref) && !payload.cursor.is_type(Token::Type::t_and)) {
        return type;
    }

    payload.cursor.skip();
    auto borrowed = AST::ValueType::make_pointer(type, false);

    // `T&&` means nothing in the language, and lexes as t_logical_and anyway
    if (payload.cursor.is_type(Token::Type::t_ref) || payload.cursor.is_type(Token::Type::t_and)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(payload.cursor.current()),
            "A reference cannot be taken twice, write 'ptr<" + type.get_type_desciption() + "&>' instead"
        );
        payload.cursor.skip();
    }

    return borrowed;
}

// parses one type, without emplacing a node. `parse_type` is the thin wrapper that turns the
// result into the single TypeNode a caller expects - recursing through parse_type instead would
// litter an orphan node into the collection for every nested level of a `ptr<ptr<T>>`
static std::optional<AST::ValueType> parse_value_type(Parser::Payload &payload)
{
    bool is_const = false;

    if (payload.cursor.is_type(Token::Type::t_const)) {
        is_const = true;
        payload.cursor.skip();
    }

    // `function<R(P...)>`, the callable type. spelled return-type-first inside the brackets, which is
    // the shape CONCEPT.md specifies
    if (Parser::starts_callable_type(payload.cursor)) {
        payload.cursor.skip(2);

        auto return_type = parse_value_type(payload);
        if (!return_type.has_value()) {
            return std::nullopt;
        }

        if (!payload.cursor.is_type(Token::Type::t_open_paren)) {
            payload.collect_unexpected_token(Token::Type::t_open_paren);
            return std::nullopt;
        }
        payload.cursor.skip();

        std::vector<AST::ValueType> parameter_types;

        while (!payload.cursor.is_type(Token::Type::t_close_paren)) {
            if (payload.cursor.is_done()) {
                payload.collect_unexpected_token(Token::Type::t_close_paren);
                return std::nullopt;
            }

            auto param = parse_value_type(payload);
            if (!param.has_value()) {
                return std::nullopt;
            }

            parameter_types.push_back(param.value());

            if (payload.cursor.is_type(Token::Type::t_comma)) {
                payload.cursor.skip();
            }
            else if (!payload.cursor.is_type(Token::Type::t_close_paren)) {
                payload.collect_unexpected_token(Token::Type::t_close_paren);
                return std::nullopt;
            }
        }
        payload.cursor.skip(); // the close paren

        // through the cursor's `>>` split, like every other argument list
        if (!payload.cursor.is_generic_close()) {
            payload.collect_unexpected_token(Token::Type::t_close_angle);
            return std::nullopt;
        }
        payload.cursor.consume_generic_close();

        auto callable = AST::ValueType::make_callable(return_type.value(), std::move(parameter_types));
        return parse_ref_suffix(payload, is_const ? AST::ValueType::make_const(callable) : callable);
    }

    // `ptr<T>` is a real type constructor, so it recurses: the pointee is an arbitrary type,
    // which is what makes ptr<ptr<T>> and ptr<Box<int>> representable at all
    if (payload.cursor.is_type(Token::Type::t_ptr)) {
        payload.cursor.skip();

        if (!payload.cursor.is_type(Token::Type::t_open_angle)) {
            payload.collect_unexpected_token(Token::Type::t_open_angle);

            return std::nullopt;
        }

        payload.cursor.skip();

        auto pointee = parse_value_type(payload);
        if (!pointee.has_value()) {
            return std::nullopt;
        }

        // is_generic_close/consume_generic_close so a nested `ptr<ptr<int>>` closing as `>>`
        // splits, exactly as it already does for nested generic applications
        if (!payload.cursor.is_generic_close()) {
            payload.collect_unexpected_token(Token::Type::t_close_angle);

            return std::nullopt;
        }

        payload.cursor.consume_generic_close();

        auto pointer_type = AST::ValueType::make_pointer(pointee.value(), true);
        return parse_ref_suffix(payload, is_const ? AST::ValueType::make_const(pointer_type) : pointer_type);
    }

    // `Owner::Nested` before `a::b::Foo`: the two spellings are indistinguishable until the leading
    // identifier is looked up, and only one of them can be resolved by descending namespaces
    std::optional<TokenReference> member_type_name;
    if (AST::TypeDeclNode *member_type = try_parse_member_type_chain(payload, member_type_name)) {
        AST::ValueType nested_type = member_type->value_type();

        // a nested type may itself be generic, and the application reads the same as any other
        if (payload.cursor.is_type(Token::Type::t_open_angle)) {
            nested_type = parse_generic_application(payload, member_type, member_type_name.value());
        }

        return parse_ref_suffix(payload, is_const ? AST::ValueType::make_const(nested_type) : nested_type);
    }

    // a type may be namespace qualified: `a::b::Foo`. the prefix is consumed here so the name
    // token below resolves in that namespace instead of the current one. a qualified name is
    // never a primitive or a type parameter, so those lookups are skipped for it
    const AST::Namespace *lookup_namespace = payload.context.declaring_namespace();
    bool is_qualified = false;
    if (payload.cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        auto *ns_node = parse_namespace(payload);
        assert(ns_node != nullptr && "expected a namespace node");
        lookup_namespace = ns_node->ast_namespace;
        is_qualified = true;
    }

    auto token = payload.cursor.current();
    auto primitive_type = is_qualified ? AST::ValueType::make_unknown() : get_primitive_type(token.value());
    AST::TypeDeclNode *user_type_decl = nullptr;

    // if it's not a primitive type, check for type parameters first
    if (!primitive_type.is_primitive() && !primitive_type.is_struct() && !primitive_type.is_class()) {
        // a generic type parameter in scope, e.g. the T of the enclosing `struct Box<T>`
        const AST::TypeParamDecl *type_param = is_qualified ? nullptr : payload.context.find_type_param(token.value());

        if (type_param) {
            primitive_type = AST::ValueType::make_type_param(type_param);
        } else {
            // check for user-defined types (structs/classes)
            auto struct_symbol = payload.collector.namespaces.find_symbol(token.value(), *lookup_namespace);
            if (struct_symbol && struct_symbol->type() == AST::SymbolType::t_type) {
                user_type_decl = struct_symbol->node.unsafe_ptr<AST::TypeDeclNode>();
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

    // generic application on a user type: `Name<Arg, Arg, ...>` (nested, e.g. Foo<Bar<int>>)
    if (user_type_decl && payload.cursor.is_type(Token::Type::t_open_angle)) {
        primitive_type = parse_generic_application(payload, user_type_decl, token);
    }

    if (is_const) {
        primitive_type = AST::ValueType::make_const(primitive_type);
    }

    return parse_ref_suffix(payload, primitive_type);
}

AST::TypeNode *Parser::parse_type(Parser::Payload &payload)
{
    auto token = payload.cursor.current();

    auto type = parse_value_type(payload);
    if (!type.has_value()) {
        return nullptr;
    }

    return &payload.context.emplace_node<AST::TypeNode>(type.value(), token);
}