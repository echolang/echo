#ifndef ASTMEMBERLOOKUP_H
#define ASTMEMBERLOOKUP_H

#pragma once

#include "AST/ASTValueType.h"

#include <string>
#include <vector>

namespace AST
{
    class FunctionDeclNode;

    // the candidate member functions a name denotes on `ct`, as an overload set - the member
    // counterpart of FunctionRegistry::overloads, and the only place that answers "which
    // declarations could `$obj->name(...)` be".
    //
    // an instantiation has no method table of its own. methods are declared on the template and
    // instantiated per call site by the monomorphizer, so a lookup on `Box<int32>` redirects
    // through template_ref and hands back the template's declarations - which are generic in the
    // owner's parameters, and bind them from the receiver argument. that redirect is why
    // TypeRegistry::get_or_create_instantiation needs to know nothing about members
    std::vector<FunctionDeclNode *> find_member_functions(const ComplexType *ct, const std::string &name);
};

#endif
