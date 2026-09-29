#include "Parser/TypeParamParser.h"
#include "Parser/TypeParser.h"

#include "AST/ASTIssue.h"
#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

#include <algorithm>
#include <optional>
#include <vector>

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

        // the list scan does not resolve atoms, so this copy clears a constraint the
        // previous pass stored. install_type_parameters re-reads the clause once the
        // names are in scope, and that re-read is the one that sticks
        decl->constraint = parsed[i].constraint;
        decl->constraint_spelling = parsed[i].constraint_spelling;
        decl->param_kind = parsed[i].param_kind;
        decl->value_type = parsed[i].value_type;
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

// re-reads each parameter's constraint clause, with `decls` already installed and the
// matching TypeParamScope already open. `decls` is the owner's own list, parallel to
// `parsed` - a method passes the tail after its inherited prefix, not the whole list.
// the type-name pass does nothing: an atom may name a type that pass has not registered.
//
// file-static so a caller cannot forget it: the public door is install_type_parameters
static void resolve_param_constraints(
    Parser::Payload &payload,
    const std::vector<Parser::ParsedTypeParam> &parsed,
    const std::vector<AST::TypeParamDecl *> &decls
)
{
    // pass 1 has not registered the types an atom may name. the declaration pass re-reads
    if (payload.pass == Parser::Pass::t_type_names) {
        return;
    }

    auto &cursor = payload.cursor;
    const Parser::Cursor::Snapshot saved = cursor.snapshot();

    const size_t count = std::min(parsed.size(), decls.size());

    for (size_t i = 0; i < count; i++) {
        if (decls[i] == nullptr) {
            continue;
        }

        if (!parsed[i].constraint_written) {
            decls[i]->constraint.clear();
            decls[i]->constraint_spelling.clear();
            continue;
        }

        cursor.restore(parsed[i].constraint_at);

        // a fresh param so a previous pass's atoms are not appended to. the name is what an
        // unknown-atom diagnostic quotes
        Parser::ParsedTypeParam resolved(parsed[i].name_token);

        if (!Parser::parse_constraint_atoms(payload, resolved, /*resolve_atoms=*/true)) {
            decls[i]->constraint.clear();
            decls[i]->constraint_spelling.clear();
            continue;
        }

        decls[i]->constraint = std::move(resolved.constraint);
        decls[i]->constraint_spelling = std::move(resolved.constraint_spelling);
    }

    cursor.restore(saved);
}

bool Parser::parse_where_clauses(Payload &payload, AST::FunctionDeclNode &funcdecl)
{
    auto &cursor = payload.cursor;
    funcdecl.where_clauses.clear();

    if (!cursor.is_type(Token::Type::t_identifier) || cursor.current().value() != "where") {
        return true;
    }

    cursor.skip();

    // the type-name pass has not registered the types an atom names. the clause is
    // still consumed, so the signature ends where the later passes expect, and stored
    // only once those types exist
    const bool resolve = payload.pass != Pass::t_type_names;

    while (true) {
        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collect_unexpected_token(Token::Type::t_identifier);
            return false;
        }

        const TokenReference name_token = cursor.current();
        const AST::TypeParamDecl *param = payload.context.find_type_param(name_token.value());

        std::optional<size_t> index;

        for (size_t i = 0; i < funcdecl.type_parameters.size(); i++) {
            if (funcdecl.type_parameters[i] == param) {
                index = i;
                break;
            }
        }

        if (!index.has_value()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(name_token),
                fmt::format(
                    "'{}' is not a type parameter of '{}', so 'where' cannot constrain it",
                    name_token.value(), funcdecl.func_name()));
            return false;
        }

        cursor.skip();

        ParsedTypeParam clause(name_token);

        if (!parse_constraint_atoms(payload, clause, resolve)) {
            return false;
        }

        if (resolve && clause.constraint.empty()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(name_token),
                fmt::format(
                    "'where {}' needs a constraint - write 'where {} : SomeType'",
                    name_token.value(), name_token.value()));
            return false;
        }

        if (resolve) {
            funcdecl.where_clauses.push_back(AST::FunctionDeclNode::WhereClause {
                param,
                *index,
                std::move(clause.constraint),
                std::move(clause.constraint_spelling)
            });
        }

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
            continue;
        }

        break;
    }

    return true;
}

AST::TypeParamScope Parser::install_type_parameters(
    Payload &payload,
    AST::FunctionDeclNode &owner,
    const std::vector<ParsedTypeParam> &parsed,
    const std::vector<AST::TypeParamDecl *> &inherited
)
{
    declare_type_parameters(payload, owner, parsed, inherited);
    AST::TypeParamScope scope(payload.context, owner.type_parameters);

    const size_t inherited_count = owner.inherited_type_param_count;
    std::vector<AST::TypeParamDecl *> own(
        owner.type_parameters.begin() + inherited_count,
        owner.type_parameters.end()
    );
    resolve_param_constraints(payload, parsed, own);
    return scope;
}

AST::TypeParamScope Parser::install_type_parameters(
    Payload &payload,
    AST::ComplexType &owner,
    const std::vector<ParsedTypeParam> &parsed
)
{
    declare_type_parameters(payload, owner, parsed);
    AST::TypeParamScope scope(payload.context, owner.type_parameters);
    resolve_param_constraints(payload, parsed, owner.type_parameters);
    return scope;
}
