#ifndef TYPEPARAMPARSER_H
#define TYPEPARAMPARSER_H

#pragma once

#include "AST/ASTContext.h"
#include "AST/ASTTypeParam.h"
#include "Parser/ParserPayload.h"

#include <string>
#include <vector>

namespace AST
{
    class FunctionDeclNode;
};

namespace Parser
{
    // one type parameter exactly as written, before it becomes a declaration. parsing produces
    // syntax; minting the owned TypeParamDecl is the declaring step, which the owner node does
    // (see AST::declare_type_parameters) so it can stay idempotent across the two parser passes
    struct ParsedTypeParam
    {
        TokenReference name_token;
        std::vector<AST::ValueType> constraint;
        std::string constraint_spelling;
        AST::TypeParamKind param_kind = AST::TypeParamKind::t_type;
        AST::ValueType value_type;

        ParsedTypeParam(
            TokenReference name_token,
            std::vector<AST::ValueType> constraint = {},
            std::string constraint_spelling = "",
            AST::TypeParamKind param_kind = AST::TypeParamKind::t_type,
            AST::ValueType value_type = {}
        ) :
            name_token(name_token),
            constraint(std::move(constraint)),
            constraint_spelling(std::move(constraint_spelling)),
            param_kind(param_kind),
            value_type(std::move(value_type))
        {}

        const std::string &name() const {
            return name_token.value();
        }

        // set when a `: ...` clause was written. the atoms are not resolved while the list is
        // being scanned: the names in this list are not declarations yet, so `T : Cmp<T>` and
        // `B : Cmp<A>` would both be unknown types. the snapshot is the colon, re-read by
        // install_type_parameters once TypeParamScope has every name in the list
        bool constraint_written = false;
        Cursor::Snapshot constraint_at {};
    };

    // `where` after the return type. the parameter names are already in scope, so
    // `where T : Cmp<T>` resolves. cleared first because both passes re-read the signature
    // and would otherwise append twice. reports and returns false; the caller owns recovery
    // (`skip_refused_function` / `skip_operator_remainder`) so a function and an operator
    // share the grammar without sharing how a refused declaration is consumed.
    //
    // a name has to be one of *this* function's type parameters. find_type_param also
    // answers an associated type and an outer parameter, and a clause on either would
    // parse and then never be judged — the checker's index is into this list
    bool parse_where_clauses(Payload &payload, AST::FunctionDeclNode &funcdecl);

    // turns parsed parameters into owned declarations installed on their owner, stamping each
    // one's ordinal and owner. idempotent across the symbol and full parser passes: an unchanged
    // list reuses the declarations already installed, so a parameter has exactly one declaration
    // no matter how often its owner is re-parsed.
    //
    // the arity-only half: the type-name pass, and a synthesized function that only inherits.
    // a declaration whose body or signature will mention the names goes through
    // install_type_parameters, which is what re-reads the colons
    void declare_type_parameters(Payload &payload, AST::ComplexType &owner, const std::vector<ParsedTypeParam> &parsed);

    // the function overload owns the whole `[inherited..., own...]` shape of
    // FunctionDeclNode::type_parameters, inherited_type_param_count included: a method of a generic
    // struct passes the owner's declarations as `inherited` and they are shared, not re-declared
    // stripping the prefix before declaring and re-prefixing after lives here rather than at the call
    // site, because the reuse rule that forces it is here. see the implementation
    void declare_type_parameters(
        Payload &payload,
        AST::FunctionDeclNode &owner,
        const std::vector<ParsedTypeParam> &parsed,
        const std::vector<AST::TypeParamDecl *> &inherited = {}
    );

    // declare the parameters, open their TypeParamScope, and re-read each constraint
    // with the names visible. the one owner of "these names are in scope and their
    // constraints are the ones written": a caller that declared then forgot to resolve
    // would compile `T : Cmp<T>` as unconstrained. the returned scope is movable so
    // it can be this function's return; it pops on destruction
    AST::TypeParamScope install_type_parameters(
        Payload &payload,
        AST::FunctionDeclNode &owner,
        const std::vector<ParsedTypeParam> &parsed,
        const std::vector<AST::TypeParamDecl *> &inherited = {}
    );

    AST::TypeParamScope install_type_parameters(
        Payload &payload,
        AST::ComplexType &owner,
        const std::vector<ParsedTypeParam> &parsed
    );
};

#endif
