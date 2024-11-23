#ifndef ASTCONTEXT_H
#define ASTCONTEXT_H

#pragma once

#include <algorithm>
#include <stdexcept>

#include "ASTModule.h"
#include "ASTFile.h"
#include "ASTCodeRef.h"
#include "ASTNamespace.h"
#include "ASTValueType.h"

namespace AST
{
    class TypeNode;
    class TypeDeclNode;
    class VarDeclNode;

    struct Context
    {
        Module &module;

        const TokenizedFile &file;

        Namespace *current_namespace;

        ScopeNode *scope_ptr = nullptr;

        // the return type of the function body being parsed, null at file scope. this is the
        // destination a `return` fits its expression to, the same way a variable declaration's
        // type is - without it a `return 0` in a `: float64` function typed its literal against
        // nothing and fell back to int32
        TypeNode *return_type_ptr = nullptr;

        // innermost-last stack of the generic type parameters currently in scope. a stack rather
        // than one flat list so a generic member of a generic struct sees both its own parameters
        // and its owner's — lookup walks outward, and leaving an inner scope restores the outer
        // one instead of wiping everything
        std::vector<std::vector<TypeParamDecl *>> type_param_scopes;

        // the struct whose body is being parsed, null everywhere else. this is what turns a
        // `function` into a *method*: parse_funcdecl reads it to bind the receiver, prefix the
        // owner's type parameters and register on the type rather than in the namespace
        //
        // carried on the context rather than passed as an argument for the same reason
        // return_type_ptr is: it flows downward through a parser that is a set of free functions
        // calling each other, and only one of them in the chain cares
        TypeDeclNode *self_struct_ptr = nullptr;

        // the receiver type `$this` binds to - the non-nullable borrow `Foo&`, or the borrow of the
        // interned self-application `Foo<T>&` for a generic owner. held as a node so every method
        // of one struct shares a single TypeNode, the way the constructor shares its return type
        TypeNode *self_type_ptr = nullptr;

        // the `$this` local of the *constructor* whose body is being parsed, null everywhere else -
        // including inside a method, whose `$this` is a borrow parameter naming storage that already
        // exists
        //
        // what it buys is one question answered where it is knowable: a write to a field of this
        // declaration is the field's *first* write. a constructor's `$this` is a fresh slot
        // gen_var_decl zero-fills, so there is no previous value owed a teardown and a `const`
        // property gets its one legitimate write - exactly what AssignNode::is_initialization means,
        // and exactly what the synthesized field-wise constructor already says about its own writes
        // said by the tree so no later pass has to infer "we are inside a constructor"
        VarDeclNode *ctor_this_ptr = nullptr;

        inline ScopeNode &scope() const {
            assert(scope_ptr);
            return *scope_ptr;
        }
        
        // push & pop the contexts scope
        void push_scope(ScopeNode &scope);
        void pop_scope();
        
        // enters a nested type-parameter scope. pushing an empty scope is fine and normal — a
        // non-generic member of a generic owner still needs its own frame so leaving it cannot
        // disturb the owner's parameters
        void push_type_param_scope(const std::vector<TypeParamDecl *> &params) {
            type_param_scopes.push_back(params);
        }

        void pop_type_param_scope() {
            assert(!type_param_scopes.empty());
            type_param_scopes.pop_back();
        }

        // resolves a name against the type parameters in scope, innermost first, so an inner
        // parameter shadows an outer one of the same name. null when the name is not a type
        // parameter here. defined out of line because it reads TypeParamDecl::name
        const TypeParamDecl *find_type_param(const std::string &name) const;

        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T &emplace_node(Args&&... args) {
            return module.nodes.emplace_back<T>(std::forward<Args>(args)...);
        }

        // same as emplace_node but will return a pointer to the node instead of a reference
        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T *emplace_nodep(Args&&... args) {
            return &module.nodes.emplace_back<T>(std::forward<Args>(args)...);
        }

        CodeRef code_ref() const {
            return CodeRef { &module, file.file, file.token_slice };
        }

        CodeRef code_ref(const TokenSlice &slice) const {
            return CodeRef { &module, file.file, slice };
        }

        CodeRef code_ref(const TokenReference &tokenref) const {
            if (!module.is_owner_of(tokenref)) {
                throw std::runtime_error("TokenReference does not belong to this module");
            }

            return CodeRef { &module, file.file, tokenref.make_slice() };
        }

        TokenReference make_virtual_token(const std::string &value, Token::Type type, size_t line, size_t char_offset) {
            auto ti = module.tokens.push(value, type, line, char_offset);
            return module.tokens[ti];
        }

        /**
         * Creates a virtual token with the same position as the reference token
         */
        TokenReference make_virtual_token(const std::string &value, Token::Type type, const TokenReference &ref) {
            return make_virtual_token(value, type, ref.line(), ref.char_offset());
        }
    };

    // scopes a type-parameter frame to a parser function, so every early return unwinds it.
    // the one guard for all declaration parsers — a hand-rolled copy per parser is how the
    // previous flat list ended up being cleared instead of restored
    struct TypeParamScope
    {
        Context &context;

        TypeParamScope(Context &context, const std::vector<TypeParamDecl *> &params) :
            context(context)
        {
            context.push_type_param_scope(params);
        }

        TypeParamScope(const TypeParamScope &) = delete;
        TypeParamScope &operator=(const TypeParamScope &) = delete;

        ~TypeParamScope() {
            context.pop_type_param_scope();
        }
    };

    // scopes the current return type to a function body, restoring the previous one rather than
    // clearing it - a declaration nested inside another body must not leak its return type back
    // out over the enclosing one
    struct ReturnTypeScope
    {
        Context &context;
        TypeNode *previous;

        ReturnTypeScope(Context &context, TypeNode *return_type) :
            context(context), previous(context.return_type_ptr)
        {
            context.return_type_ptr = return_type;
        }

        ReturnTypeScope(const ReturnTypeScope &) = delete;
        ReturnTypeScope &operator=(const ReturnTypeScope &) = delete;

        ~ReturnTypeScope() {
            context.return_type_ptr = previous;
        }
    };

    // scopes the enclosing struct to a struct body, so a `function` inside one parses as a method
    //
    // saves and restores rather than clearing, like ReturnTypeScope, and for the sharper version of
    // the same reason: parse_funcdecl opens a *null* frame around the body it parses, so a
    // declaration nested inside a method does not inherit a receiver it has no business having
    struct SelfScope
    {
        Context &context;
        TypeDeclNode *previous_struct;
        TypeNode *previous_type;

        SelfScope(Context &context, TypeDeclNode *self_struct, TypeNode *self_type) :
            context(context),
            previous_struct(context.self_struct_ptr),
            previous_type(context.self_type_ptr)
        {
            context.self_struct_ptr = self_struct;
            context.self_type_ptr = self_type;
        }

        SelfScope(const SelfScope &) = delete;
        SelfScope &operator=(const SelfScope &) = delete;

        ~SelfScope() {
            context.self_struct_ptr = previous_struct;
            context.self_type_ptr = previous_type;
        }
    };

    // scopes a constructor's `$this` to that constructor's body, so a write to one of its fields is
    // recognised as the field's first write
    //
    // saves and restores for the same reason SelfScope does, and it matters here for a sharper case:
    // a `function` declared inside a constructor body opens a *null* frame, so a write to some other
    // struct's field in there is an ordinary replacement rather than an initialization
    struct ConstructorScope
    {
        Context &context;
        VarDeclNode *previous_this;

        ConstructorScope(Context &context, VarDeclNode *ctor_this) :
            context(context),
            previous_this(context.ctor_this_ptr)
        {
            context.ctor_this_ptr = ctor_this;
        }

        ConstructorScope(const ConstructorScope &) = delete;
        ConstructorScope &operator=(const ConstructorScope &) = delete;

        ~ConstructorScope() {
            context.ctor_this_ptr = previous_this;
        }
    };
};
#endif