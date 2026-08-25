#include "Parser/TypeParser.h"
#include "Parser/VisibilityParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/IfStatementParser.h"
#include "AST/ASTImport.h"
#include "AST/ASTNullability.h"
#include "AST/ASTValueType.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"
#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>
#include <fmt/core.h>

namespace
{
    struct TypeNameSite
    {
        TokenReference token;
        AST::ValueType type;

        TypeNameSite(const TokenReference &token, AST::ValueType type)
            : token(token), type(std::move(type))
        {}
    };
};

// the type grammar is mutually recursive: a generic argument is a type, and a type may be a
// generic application. both work on bare ValueTypes, only the public entry point makes a node
static std::optional<AST::ValueType> parse_value_type(
    Parser::Payload &payload,
    std::vector<TypeNameSite> *names
);

AST::Symbol *Parser::find_unqualified_type(Parser::Payload &payload, const std::string &name, const AST::Namespace &from)
{
    if (const AST::ImportBinding *imp = AST::item_import_for(
            AST::file_of(payload.context), payload.collector, name)) {
        return payload.collector.namespaces.find_symbol(imp->target_name, *imp->target_namespace);
    }

    return payload.collector.namespaces.find_symbol_in_scope(name, from);
}

bool Parser::starts_callable_type(Parser::Cursor &cursor, size_t offset)
{
    // the `<` is what tells it from a *declaration*, which has an identifier there, and from a closure
    // literal, which has a `(`. one token of lookahead, and the three spellings cannot overlap
    return cursor.peek_is_type(offset, Token::Type::t_function) &&
        cursor.peek_is_type(offset + 1, Token::Type::t_open_angle);
}

bool Parser::starts_c_function_type(Parser::Cursor &cursor, size_t offset)
{
    return cursor.peek_is_type(offset, Token::Type::t_extern)
        && starts_callable_type(cursor, offset + 1);
}

bool Parser::can_parse_type(Parser::Payload &payload)
{
    // a type can be preceded by a const keyword
    size_t offset = 0;
    if (payload.cursor.is_type(Token::Type::t_const)) {
        offset++;
    }

    // a type can be an identifier, or one of the two type constructors the compiler spells
    if (
        payload.cursor.peek_is_type(offset, Token::Type::t_identifier) ||
        payload.cursor.peek_is_type(offset, Token::Type::t_ptr) ||
        payload.cursor.peek_is_type(offset, Token::Type::t_weak)
    ) {
        return true;
    }

    return starts_c_function_type(payload.cursor, offset)
        || starts_callable_type(payload.cursor, offset);
}

// advances past one type if the tokens at the cursor form one, and answers whether they did
//
// shape only - no symbol lookup, no diagnostics, no nodes - because this runs at a statement head,
// where the answer decides which parser is called at all. a type name is not necessarily resolvable
// there either: the type-name pass may not have reached it, and an unresolved unqualified name is
// silently `unknown` rather than a diagnostic
//
// the grammar it walks mirrors parse_value_type:
//   type      := 'const'? ( 'ptr' '<' type '>' | 'weak' '<' type '>' | qualified ) nullable? ref?
//   qualified := ( identifier '::' )* identifier ( '<' type ( ',' type )* '>' )?
//   nullable  := '?'
//   ref       := '&'
// it consumes its closing angle brackets through the cursor's own '>>' split rather than counting
// them, so `Box<Box<int32>>` cannot end in a different place here than it does in
// parse_generic_application - two notions of "where does this argument list end" would desync
//
// only the bounds-safe cursor accessors are used (never current(), which asserts), so a truncated
// type runs out of tokens and answers false instead of aborting
// **the one place a `?` suffix is consumed.** two walks reach it - skip_type_shape, which only decides
// *which parser runs*, and parse_nullable_suffix, which builds the type - and they have to agree on how
// many `?`s a type ends with. when they did not, `Foo? $x` scanned as a declaration in one and not the
// other, and the disagreement was silent because neither reports anything about the suffix
//
// idempotent: `T??` is `T?`. there is one `null` in the language, so a second level of absence has
// nothing to mean - deliberately the opposite of `ptr<ptr<T>>`, where a second level is a different type
static bool skip_nullable_suffix(Parser::Cursor &cursor)
{
    bool seen = false;

    while (cursor.is_type(Token::Type::t_qmark)) {
        cursor.skip();
        seen = true;
    }

    return seen;
}

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

    // `extern function<R(P...)>` is the callable shape with one more word in front. skip the
    // `extern` and fall into the same walk, so the two cannot disagree about where the type ends
    if (Parser::starts_c_function_type(cursor)) {
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
    // `ptr<T>` and `weak<T>` are type constructors, so they recurse on their argument exactly as the
    // parser does. one arm because their *shape* is identical - what differs is only which ValueType
    // parse_value_type builds, and whether the argument has to be a class, neither of which is a shape
    else if (cursor.is_type(Token::Type::t_ptr) || cursor.is_type(Token::Type::t_weak)) {
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

        // a generic application, `Foo<Arg, Arg>`. this needs no reinterpretation the way `->name<` does, and
        // no longer because a bare identifier could not be a value operand - a compile-time constant is one,
        // so `MAX < $n` reaches here at a statement head. It declines anyway, and for a sturdier reason: the
        // list below is a list of *type shapes*, and a `$n` is not one, so the scan fails and the statement
        // is not read as a declaration
        if (cursor.is_type(Token::Type::t_open_angle)) {
            cursor.skip();

            if (!skip_type_list(cursor, [](Parser::Cursor &c) { return c.is_generic_close(); })) {
                return false;
            }

            cursor.consume_generic_close();
        }
    }

    // the nullable suffix, then the borrow suffix, in that order and in either of the two spellings the
    // lexer produces for the latter (see parse_ref_suffix). the order has to match parse_value_type's
    skip_nullable_suffix(cursor);

    if (cursor.is_type(Token::Type::t_ref) || cursor.is_type(Token::Type::t_and)) {
        cursor.skip();
    }

    return true;
}

bool Parser::constdecl_omits_its_type(Parser::Cursor &cursor)
{
    // `NAME =` or `NAME ;`. The second is a constant with no value, claimed here so that parse_constdecl is
    // the one that gets to say so rather than the type parser failing on a `;`
    return cursor.is_type(Token::Type::t_identifier)
        && (cursor.peek_is_type(1, Token::Type::t_assign) || cursor.peek_is_type(1, Token::Type::t_semicolon));
}

bool Parser::starts_constdecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_const)) {
        return false;
    }

    // the scan moves the cursor and puts it back, snapshot-restored for skip_type_shape's '>>' reason
    const auto snapshot = cursor.snapshot();
    cursor.skip(); // the `const`

    // **the typed form first.** `const usize MAX` - a whole type followed by a bare identifier.
    //
    // the order is the content: on `const MAX = 1` the shape scan happily eats `MAX` as a type name and
    // then finds `=` rather than an identifier, so it declines and the untyped arm below answers. Reversed,
    // the untyped arm would never be reached for anything typed
    const auto after_const = cursor.snapshot();
    if (skip_type_shape(cursor) && cursor.is_type(Token::Type::t_identifier)) {
        cursor.restore(snapshot);
        return true;
    }
    cursor.restore(after_const);

    // the untyped form, through the same predicate parse_constdecl reads to decide whether to parse a type -
    // one answer to "which spelling is this", not two that could drift
    const bool is_const_decl = constdecl_omits_its_type(cursor);

    cursor.restore(snapshot);

    return is_const_decl;
}

bool Parser::starts_vardecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    // **`static` is a modifier and never the declaration**, so it is skipped and the question asked of
    // what follows. it is safe to skip unconditionally: `static` is a reserved word, so nothing else in
    // the grammar can begin with one, and `static function` was already claimed by starts_funcdecl -
    // which the struct-body dispatch asks ahead of this, exactly as it asks starts_constdecl
    //
    // scanned rather than consumed, because all four predicates read from the statement's head and a
    // consumed modifier would leave the three that answer no looking at the wrong token
    if (cursor.is_type(Token::Type::t_static)) {
        const auto before_modifier = cursor.snapshot();
        cursor.skip();
        const bool is_decl = starts_vardecl(payload);
        cursor.restore(before_modifier);

        return is_decl;
    }

    // `const`, `ptr` and `weak` begin nothing else at a statement head, so they answer without a scan.
    // that keeps a malformed one reported by parse_varexpr, which knows what it was reading,
    // rather than by the statement dispatch's catch-all
    //
    // `weak` is only *almost* true here: `weak($obj)` is an expression too. it is still safe, because a
    // declaration is the reading that needs the whole statement and the expression form is reached from
    // parse_varexpr either way - see the arm in ExprParser
    // `const` is the one of the three that begins something else after all - and it begins **two** other
    // things, not one. a **compile-time constant**, which differs only in carrying no `$` on its name, and
    // a **compile-time branch**, `const if (...)`. Deferring to both predicates rather than answering yes
    // outright is what keeps the three a genuine partition - every dispatch site asks the other two first,
    // and a site that did not would otherwise read `const MAX = 100;` as a declaration whose name token is
    // a `=`, or `const if (X) { }` as one whose name token is a `(`
    if (cursor.is_type(Token::Type::t_const)) {
        return !starts_constdecl(payload) && !starts_const_if(cursor);
    }

    if (cursor.is_type(Token::Type::t_ptr)
        || cursor.is_type_sequence(0, { Token::Type::t_weak, Token::Type::t_open_angle })) {
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
//
// a generic *application* (`contract::iterable<int32>`) is not resolved here - it is handed to
// Parser::parse_type by the caller, which owns every spelling of a type including the arity check and the
// `>>` split. this stays the bare-name rule
static std::optional<std::vector<AST::ValueType>> resolve_constraint_atom(Parser::Payload &payload, const std::string &name)
{
    if (auto alias = expand_type_alias(name)) {
        return alias;
    }

    auto primitive = get_primitive_type(name);
    if (primitive.is_primitive()) {
        return std::vector<AST::ValueType>{ primitive };
    }

    // the namespace this constraint is *written* in, searched outward - so a block-local type is
    // nameable inside the block that declared it and a file-scope one still resolves from inside a block
    auto symbol = find_unqualified_type(payload, name, *payload.context.current_namespace);
    if (symbol && symbol->type() == AST::SymbolType::t_type) {
        auto *decl = symbol->node.unsafe_ptr<AST::TypeDeclNode>();
        refuse_invisible_type(payload, *decl, payload.cursor.current());
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
static AST::TypeDeclNode *try_parse_member_type_chain(
    Parser::Payload &payload,
    std::optional<TokenReference> &out_name,
    std::vector<TypeNameSite> *names
)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        return nullptr;
    }

    // the namespace this type is written in, the same rule resolve_constraint_atom above states.
    // searched *outward* from it, so an owner written unqualified is found wherever it is in scope -
    // including from inside another nested type's body, which parses in a namespace named after its own
    // owner, and including a block-local owner declared beside this use
    auto *symbol = find_unqualified_type(
        payload, cursor.current().value(), *payload.context.current_namespace);

    if (symbol == nullptr || symbol->type() != AST::SymbolType::t_type) {
        return nullptr;
    }

    AST::TypeDeclNode *owner = symbol->node.unsafe_ptr<AST::TypeDeclNode>();

    refuse_invisible_type(payload, *owner, cursor.current());

    // snapshot/restore, the idiom parse_member_call and starts_vardecl already use for exactly this
    // ambiguity: a name can be both a type and a namespace, and every failure below leaves the caller
    // to try the namespace-qualified path. consuming irreversibly here left the cursor mid-name and
    // turned one mis-resolved `A::B` into a cascade of unrelated diagnostics
    const TokenReference owner_token = cursor.current();
    const auto start = cursor.snapshot();

    cursor.skip(); // the owner name
    cursor.skip(); // the `::`

    std::vector<TypeNameSite> pending;
    pending.emplace_back(owner_token, owner->value_type());

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
        pending.emplace_back(name_token, nested->value_type());
        out_name.emplace(name_token); // TokenReference has no copy assignment
        cursor.skip();

        // another `::` continues the chain
        if (!cursor.is_type_sequence(0, { Token::Type::t_namespace_sep, Token::Type::t_identifier })) {
            if (names != nullptr) {
                for (const TypeNameSite &site : pending) {
                    names->emplace_back(site.token, site.type);
                }
            }

            return owner;
        }

        cursor.skip(); // the `::`
    }
}

// parses `<Arg, Arg, ...>` (cursor positioned at the opening `<`) as generic type arguments
// applied to `template_decl`, and returns the interned application ValueType. Arguments are
// themselves types, so nesting (Foo<Bar<int>>) falls out of the recursion into parse_type
static AST::ValueType parse_generic_application(
    Parser::Payload &payload,
    AST::TypeDeclNode *template_decl,
    const TokenReference &name_token,
    std::vector<TypeNameSite> *names
)
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

        auto arg_type = parse_value_type(payload, names);
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

// a *generic* type named bare is refused rather than pushed: it resolves to the template, which no
// value's type ever equals and no conformance ever names, so the constraint would reject every argument
// with the atom looking perfectly correct. `iterable<int32>` is the spelling that works
//
// both resolving arms ask. an *applied* atom always interns an instantiation and can never trip it, but
// a qualified atom reaches parse_type whether it was applied or not
static bool constraint_atom_is_bare_generic(const AST::ValueType &type)
{
    return type.has_complex_type() && type.get_complex_type()->is_generic();
}

static void collect_bare_generic_refusal(
    Parser::Payload &payload,
    const TokenReference &atom_token,
    const std::string &spelling,
    const std::string &param_name
)
{
    payload.collector.collect_issue<AST::Issue::GenericError>(
        payload.context.code_ref(atom_token),
        fmt::format(
            "'{}' is generic, so it needs its type arguments in the constraint of "
            "type parameter '{}' - write '{}<...>'.",
            spelling, param_name, spelling)
    );
}

static void append_constraint_spelling(Parser::ParsedTypeParam &param, const std::string &spelling)
{
    if (!param.constraint_spelling.empty()) {
        param.constraint_spelling += "|";
    }
    param.constraint_spelling += spelling;
}

// **the constraint grammar**, `: atom (| atom)*`, as one function.
//
// `class` is an atom too — a keyword, not an identifier, which is why the walk claims
// `t_class` ahead of the identifier arm. `numeric` is an identifier alias; this one is not.
//
// two callers: a type parameter's constraint in parse_type_param_list, and an interface's associated
// type in parse_typedecl. one grammar, so the `>>` split, the deferred resolution in the type-name
// pass and the bare-generic refusal cannot drift between the two spellings
//
// hands back false when it reported and gave up, so the caller can abandon whatever list it was
// building. does nothing and answers true when the cursor is not on a ':'
bool Parser::parse_constraint_atoms(Parser::Payload &payload, ParsedTypeParam &param)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_colon)) {
        return true;
    }

    cursor.skip(); // skip ':'

    // the type-name pass reads this list for the one thing it owns - a generic type's arity
    // and its parameter names - and a constraint atom may name a type no pass has registered
    // yet, so resolving one here would report an unknown type for a well formed program. the
    // atoms are still walked, to leave the cursor after the list, and the declaration pass
    // fills the constraint in: declare_params refreshes it on every pass precisely so this
    // can be deferred
    const bool resolve_atoms = payload.pass != Pass::t_type_names;

    while (true) {
        // `class` is a keyword, so it is not an identifier. `numeric` is. this atom is the
        // one kind predicate that is spelled with a keyword, and it has to be claimed before
        // the identifier arm or the unexpected-token report names the wrong thing
        if (cursor.is_type(Token::Type::t_class)) {
            if (resolve_atoms) {
                param.constraint.push_back(AST::ValueType::make_class_kind_constraint());
                append_constraint_spelling(param, "class");
            }

            cursor.skip();
        } else if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collect_unexpected_token(Token::Type::t_identifier);
            cursor.try_skip_to_next_statement();
            return false;
        } else {
            auto atom_token = cursor.current();
            auto atom_name = atom_token.value();

            // an atom may be a generic *application* - `C: iterable<int32>` - and not only a bare
            // name. an interface is what made that worth having: `iterable` alone resolves to the
            // template, which is not a type any value ever has, so a constraint naming one could
            // never be satisfied by anything
            //
            // it may also be namespace qualified - `C: contract::iterable<int32>`. the `<` that decides
            // "application" sits after the prefix, so the peek has to reach past it, and a qualified atom
            // goes to parse_type whether or not it is applied: an alias and a primitive are unqualified
            // spellings, which is the whole of what resolve_constraint_atom answers and parse_type does not
            const size_t name_offset = Parser::peek_past_namespace_prefix(payload, 0);
            const bool is_qualified = name_offset > 0;
            const bool is_application = cursor.peek_is_type(name_offset + 1, Token::Type::t_open_angle);

            if (!resolve_atoms) {
                // walked with skip_type_shape, the one owner of "how far does a written type
                // extend" - a second scanner counting angle brackets is a second answer to where
                // `contract::iterable<Box<int32>>` ends, and the cursor's `>>` split is exactly what the
                // resolving arm below reaches through parse_type. this pass validates nothing, so a
                // shape it cannot walk is left for the declaration pass to report
                skip_type_shape(cursor);
            }
            else if (is_qualified || is_application) {
                // parse_type owns every spelling of a type - the arity check against the template, the
                // interning, the namespace prefix, and the `>>` split its closing brackets need - so an
                // applied or qualified atom is read by it rather than by a second scanner here. it leaves
                // the cursor after the type, which is why this arm does not skip
                AST::TypeNode *parsed = Parser::parse_type(payload);

                if (parsed == nullptr) {
                    // parse_type has reported
                    cursor.try_skip_to_next_statement();
                    return false;
                }

                const std::string spelling = parsed->type.get_type_desciption();

                if (constraint_atom_is_bare_generic(parsed->type)) {
                    collect_bare_generic_refusal(payload, atom_token, spelling, param.name());
                    cursor.try_skip_to_next_statement();
                    return false;
                }

                param.constraint.push_back(parsed->type);
                append_constraint_spelling(param, spelling);
            }
            else {
                auto atom_types = resolve_constraint_atom(payload, atom_name);
                if (!atom_types) {
                    payload.collector.collect_issue<AST::Issue::GenericError>(
                        payload.context.code_ref(atom_token),
                        fmt::format("Unknown type or alias '{}' in constraint of type parameter '{}'", atom_name, param.name())
                    );
                    cursor.try_skip_to_next_statement();
                    return false;
                }

                if (atom_types->size() == 1 && constraint_atom_is_bare_generic(atom_types->front())) {
                    collect_bare_generic_refusal(payload, atom_token, atom_name, param.name());
                    cursor.try_skip_to_next_statement();
                    return false;
                }

                for (const auto &type : *atom_types) {
                    param.constraint.push_back(type);
                }
                append_constraint_spelling(param, atom_name);

                cursor.skip();
            }
        }

        // more atoms are separated by '|'
        if (cursor.is_type(Token::Type::t_or)) {
            cursor.skip();
            continue;
        }
        break;
    }

    return true;
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

    while (!cursor.is_generic_close()) {
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
        if (!parse_constraint_atoms(payload, param)) {
            return type_parameters;
        }

        type_parameters.push_back(std::move(param));

        // comma separator or closing angle
        //
        // is_generic_close/consume_generic_close rather than a plain t_close_angle test: a constraint
        // atom may now be a generic *application*, so this list can be the outer one of a `>>` pair -
        // `<C: contract::iterable<int32>>` ends in one token that closes both levels. every other closing
        // angle in the parser already goes through the cursor's split for this reason; this list had no way
        // to be an outer level until an applied atom existed
        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        } else if (!cursor.is_generic_close()) {
            payload.collect_unexpected_token(Token::Type::t_close_angle);
            cursor.try_skip_to_next_statement();
            return type_parameters;
        }
    }

    cursor.consume_generic_close();
    return type_parameters;
}

// mints (or reuses) the owned declarations for a freshly parsed parameter list
//
// reuse matters for correctness, not just allocation: a module is parsed twice, a symbol pass
// then a full pass, each with a fresh Context, and both reach this point for the same list
// minting new declarations the second time would give the two passes distinct parameters, so a
// generic struct's self-application Foo<T> would intern twice and the two Foo<T> would compare
// unequal. reusing whenever the shape is unchanged keeps a single declaration per parameter
static std::vector<AST::TypeParamDecl *> declare_params(
    Parser::Payload &payload,
    const std::vector<AST::TypeParamDecl *> &existing,
    const std::vector<Parser::ParsedTypeParam> &parsed
)
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
    const std::vector<AST::TypeParamDecl *> &inherited
)
{
    // the function's *own* parameters, with any inherited prefix taken off first. it has to come off:
    // declare_params decides whether it can reuse the existing declarations by comparing list
    // *sizes*, and the second parse pass reaches this node with the prefix already in place - left
    // there the sizes would mismatch and the own parameters would be re-minted, giving the two
    // passes distinct declarations, which is exactly what the reuse rule exists to prevent
    std::vector<AST::TypeParamDecl *> own(
        owner.type_parameters.begin() + owner.inherited_type_param_count,
        owner.type_parameters.end()
    );

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
// arrives as t_and and would lose its reference silently. in type position a following `&`
// can never be a binary operator, so accepting both is unambiguous and needs no lexer change -
// which matters, because the binary/reference distinction elsewhere depends on that rule
// `T?` - the nullable suffix, read before the borrow suffix so it binds to the type it is written on:
// `Foo?&` is a borrow of a `Foo?`, which is the only grouping either spelling could sensibly have
//
// there is deliberately no `T&?`. a nullable borrow already has a name and has had one since before this
// existed - it is `ptr<T>`, and that *is* this same flag on a pointer level. giving it a second spelling
// would be two ways to write one type, which is the thing generalising the flag was meant to end
static AST::ValueType parse_nullable_suffix(Parser::Payload &payload, AST::ValueType type)
{
    if (!skip_nullable_suffix(payload.cursor)) {
        return type;
    }

    // **the `?` is one spelling of two implementations**, and which one it is, is the payload's to decide.
    // over an address whose null value already means absent it is a flag and costs nothing; over anything
    // else it is a tagged pair, and a pair is a type this registry interns. AST::ValueType::has_null_representation
    // owns that split, and both entry points assert their half of it
    return payload.collector.type_registry.get_or_create_optional(type);
}

static AST::ValueType parse_ref_suffix(Parser::Payload &payload, AST::ValueType type)
{
    type = parse_nullable_suffix(payload, type);

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
// litter an orphan node into the collection for every nested level of a `ptr<ptr<T>>`.
// `names` collects each identifier this spelling resolved, so an editor walk does not re-parse
static std::optional<AST::ValueType> parse_value_type(
    Parser::Payload &payload,
    std::vector<TypeNameSite> *names
)
{
    bool is_const = false;

    if (payload.cursor.is_type(Token::Type::t_const)) {
        is_const = true;
        payload.cursor.skip();
    }

    // `extern function<R(P...)>`, the C function-pointer type. the `extern` is the calling shape;
    // the interior is the same signature the callable arm below walks, so they share the rest
    const bool c_function = Parser::starts_c_function_type(payload.cursor);
    if (c_function) {
        payload.cursor.skip();
    }

    // `function<R(P...)>`, the callable type. spelled return-type-first inside the brackets, which is
    // the shape CONCEPT.md specifies
    if (Parser::starts_callable_type(payload.cursor)) {
        payload.cursor.skip(2);

        auto return_type = parse_value_type(payload, names);
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

            auto param = parse_value_type(payload, names);
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

        auto built = c_function
            ? AST::ValueType::make_c_function(return_type.value(), std::move(parameter_types))
            : AST::ValueType::make_callable(return_type.value(), std::move(parameter_types));
        return parse_ref_suffix(payload, is_const ? AST::ValueType::make_const(built) : built);
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

        auto pointee = parse_value_type(payload, names);
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

    // `weak<T>`, the non-owning handle on a counted object. the same shape as `ptr<T>` above and
    // deliberately spelled like it - both are type constructors the compiler owns rather than library
    // types, which is what the lowercase says
    if (payload.cursor.is_type(Token::Type::t_weak)) {
        auto weak_token = payload.cursor.current();
        payload.cursor.skip();

        if (!payload.cursor.is_type(Token::Type::t_open_angle)) {
            payload.collect_unexpected_token(Token::Type::t_open_angle);

            return std::nullopt;
        }

        payload.cursor.skip();

        auto target = parse_value_type(payload, names);
        if (!target.has_value()) {
            return std::nullopt;
        }

        if (!payload.cursor.is_generic_close()) {
            payload.collect_unexpected_token(Token::Type::t_close_angle);

            return std::nullopt;
        }

        payload.cursor.consume_generic_close();

        // **only a class may be weakly referenced**, because only a class is counted - there is nothing
        // for a weak reference to a struct or an `int32` to be non-owning *of*. reported here, at the
        // type, rather than left to make_weak's assert: this is the one place that has a token to point at
        //
        // a type parameter is admitted and checked after substitution, the way every other constraint on
        // a parameter is - `weak<T>` inside a template is exactly how a generic cache spells its entries
        if (!target->is_class() && !AST::is_undetermined_type(target.value())) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(weak_token),
                fmt::format(
                    "'weak<{}>' is not a weak reference to anything - only a class is reference counted, "
                    "so only a class has a count to opt out of",
                    target->get_type_desciption()));

            return std::nullopt;
        }

        // an undetermined target cannot be built into a weak yet, and saying so is not this pass's job -
        // the monomorphizer reports whatever never resolved. handing the target back unchanged keeps the
        // parse going without minting a type whose invariant make_weak would refuse
        if (!target->is_class() && !target->is_type_param()) {
            return parse_ref_suffix(payload, target.value());
        }

        auto weak_type = AST::ValueType::make_weak(target.value());
        return parse_ref_suffix(payload, is_const ? AST::ValueType::make_const(weak_type) : weak_type);
    }

    // `Owner::Nested` before `a::b::Foo`: the two spellings are indistinguishable until the leading
    // identifier is looked up, and only one of them can be resolved by descending namespaces
    std::optional<TokenReference> member_type_name;
    if (AST::TypeDeclNode *member_type = try_parse_member_type_chain(payload, member_type_name, names)) {
        AST::ValueType nested_type = member_type->value_type();

        // a nested type may itself be generic, and the application reads the same as any other
        if (payload.cursor.is_type(Token::Type::t_open_angle)) {
            nested_type = parse_generic_application(payload, member_type, member_type_name.value(), names);
            if (names != nullptr && !names->empty()) {
                names->back().type = nested_type;
            }
        }

        return parse_ref_suffix(payload, is_const ? AST::ValueType::make_const(nested_type) : nested_type);
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
    AST::TypeDeclNode *user_type_decl = nullptr;

    // if it's not a primitive type, check for type parameters first
    if (!primitive_type.is_primitive() && !primitive_type.is_struct() && !primitive_type.is_class()) {
        // a generic type parameter in scope, e.g. the T of the enclosing `struct Box<T>`
        const AST::TypeParamDecl *type_param = is_qualified ? nullptr : payload.context.find_type_param(token.value());

        if (type_param) {
            primitive_type = AST::ValueType::make_type_param(type_param);
        } else {
            // check for user-defined types (structs/classes). an unqualified name is searched from
            // the enclosing namespace *outward*, the way an unqualified call already resolves; a
            // qualified one names exactly one namespace and must not fall back to an outer type of
            // the same name, or `geometry::Point` would quietly answer with the root's `Point`
            auto struct_symbol = is_qualified
                ? payload.collector.namespaces.find_symbol(token.value(), *lookup_namespace)
                : find_unqualified_type(payload, token.value(), *lookup_namespace);

            if (struct_symbol && struct_symbol->type() == AST::SymbolType::t_type) {
                user_type_decl = struct_symbol->node.unsafe_ptr<AST::TypeDeclNode>();
                primitive_type = user_type_decl->value_type();

                // reported, and the type still handed back - see Parser::refuse_invisible_type
                refuse_invisible_type(payload, *user_type_decl, token);
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
        primitive_type = parse_generic_application(payload, user_type_decl, token, names);
    }

    if (names != nullptr) {
        names->emplace_back(token, primitive_type);
    }

    if (is_const) {
        primitive_type = AST::ValueType::make_const(primitive_type);
    }

    return parse_ref_suffix(payload, primitive_type);
}

AST::TypeNode *Parser::parse_type(Parser::Payload &payload)
{
    auto token = payload.cursor.current();
    std::vector<TypeNameSite> names;

    auto type = parse_value_type(payload, &names);
    if (!type.has_value()) {
        return nullptr;
    }

    AST::TypeNode &node = payload.context.emplace_node<AST::TypeNode>(type.value(), token);
    for (const TypeNameSite &site : names) {
        AST::TypeNode &leaf = payload.context.emplace_node<AST::TypeNode>(site.type, site.token);
        node.written_names.push_back(&leaf);
    }

    return &node;
}
