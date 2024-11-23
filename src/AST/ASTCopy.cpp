#include "AST/ASTCopy.h"

#include "AST/ASTDestruction.h"
#include "AST/ASTMemberLookup.h"

AST::FunctionDeclNode *AST::copy_constructor_for(const AST::ValueType &type)
{
    if (!type.has_complex_type()) {
        return nullptr;
    }

    return AST::find_copy_constructor(type.get_complex_type());
}

bool AST::copy_needs_constructor(const AST::ValueType &type)
{
    // the declared answer first, and deliberately not gated on ownership: a type that says how it is
    // copied is copied that way, so the explicit `Foo($a)` and the implicit `$b = $a` cannot diverge
    if (AST::copy_constructor_for(type) != nullptr) {
        return true;
    }

    // and the one the compiler asks on its own behalf: an owning value has no byte copy, whether or
    // not its author has said what a real one would be
    return AST::needs_destruction(type);
}
