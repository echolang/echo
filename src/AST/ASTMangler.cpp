#include "AST/ASTMangler.h"

#include "AST/ASTNamespace.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeNode.h"

AST::mangled_id_t AST::mangle_function_name(const AST::FunctionDeclNode *func_decl)
{
    // an extern declaration links against a symbol somebody else emitted, so it is the one kind
    // of function whose name must survive untouched - no namespace prefix, no argument types
    // checked first so nothing downstream can decorate it
    if (func_decl->extern_symbol.has_value()) {
        return func_decl->extern_symbol.value();
    }

    std::string mangled_name = "_";

    // root first, so a nested namespace reads in declaration order and the root contributes
    // no empty segment
    //
    // the *mangling* segments, not the display ones: a lexical namespace names a `{ }` block, and two
    // blocks of one function both display as that function's name. taking the display path here would
    // give both their `helper(int32)` one symbol, and two bodies would be emitted into one
    // llvm::Function - the mangling defect TypeLowering throws on
    if (func_decl->ast_namespace) {
        for (const auto &segment : func_decl->ast_namespace->mangling_segments()) {
            mangled_name += segment + "_";
        }
    }

    // a member function is qualified by the type it belongs to. without this a method
    // `Foo::get()` and a free `get(Foo& $f)` mangle identically - and a method is deliberately
    // absent from the (namespace, name) overload sets, so DuplicateFunctionSignature cannot see
    // the clash and it would surface as TypeLowering's "this is a name mangling defect, not a
    // source error" throw instead. 'M' for the same reason 'G' is not another 'Z': an owner
    // segment can never be read as a namespace segment or a parameter
    //
    // mangled_token() already carries the owner's namespace path and, for an instantiation, its
    // type arguments - so `a::Box<int32>::get` and `b::Box<float64>::get` separate
    if (func_decl->owner_type != nullptr) {
        mangled_name += "M" + func_decl->owner_type->mangled_token() + "_";
    }

    mangled_name += func_decl->func_name() + "Z";

    for (auto arg : func_decl->args) {
        mangled_name += "Z" + arg->type_node()->type.get_mangled_name();
    }

    // the type arguments an instance was created with, in declaration order. the argument types
    // above are not enough on their own: a generic whose parameter appears only in the return
    // type - `alloc<T>(usize) : ptr<T>` - mangles identically for every instantiation without
    // this, and two bodies then get emitted into one llvm::Function. 'G' rather than another 'Z'
    // so a type argument can never be mistaken for a parameter
    for (const auto &type_arg : func_decl->instantiation_args) {
        mangled_name += "G" + type_arg.get_mangled_name();
    }

    return mangled_name;
}

AST::mangled_id_t AST::mangle_function_name(const AST::FunctionCallExprNode *func_call)
{
    return func_call->decorated_func_name();
}
