#ifndef FUNCTIONNODE_H
#define FUNCTIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"

#include "ScopeNode.h"
#include "VarDeclNode.h"
#include "AttributeNode.h"

#include <optional>

namespace AST 
{
    class Namespace;
    class AttributeNode;

    class FunctionDeclNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_func_decl);
            
        std::optional<TokenReference> name_token;
        std::vector<VarDeclNode*> args;
        
        // this function's own generic type parameters (the T, U in `function name<T, U>(...)`),
        // owned by the collector's TypeParamRegistry. a method of a generic struct shares the
        // struct's declarations rather than copying them, so a substitution built from either
        // list binds the same parameters. cleared on a clone, which is concrete by definition
        std::vector<TypeParamDecl *> type_parameters;

        // the mirror of ComplexType::template_ref / instantiation_args, for functions: on an
        // instance created by the monomorphizer these name the template it came from and the
        // concrete types it was instantiated with, in declaration order. empty on a template and
        // on a plain non-generic function.
        //
        // load-bearing for the mangled name. `decorated_func_name` mangles the argument types
        // only, so a generic whose parameter appears solely in the *return* type - which is
        // exactly `mem::alloc<T>(usize) : ptr<T>` - produced one symbol for every instantiation.
        // two bodies then landed in one llvm::Function. under opaque pointers every ptr<T>
        // lowers to the same llvm type, so the IR verifier could not even see it
        std::vector<ValueType> instantiation_args;
        FunctionDeclNode *template_ref = nullptr;

        inline bool is_instantiated() const {
            return template_ref != nullptr;
        }

        TypeNode *return_type = nullptr;
        Namespace *ast_namespace = nullptr;
        ScopeNode *body = nullptr;
        
        // the list of attributes that are attached to this function
        AttributeList attributes;

        // A function can be marked as intrinsic, meaning it is implemented in the compiler
        // the string represents the name of the intrinsic function to be called
        // those function must be mapped by the compiler, unknown intrinsic functions will
        // result in a compile error
        std::optional<std::string> intrinsic;

        // declared inside an `extern { }` block: the function lives in another object file and
        // this is the raw symbol to link against. it bypasses name mangling entirely - the whole
        // point is that `malloc` is spelled `malloc` in the symbol table - so an extern
        // declaration is necessarily non-generic and bodyless, both of which the parser enforces
        std::optional<std::string> extern_symbol;

        inline bool is_extern() const {
            return extern_symbol.has_value();
        }

        // marked `#[builtin: "size_of"]`: the compiler answers a call to this function directly
        // instead of emitting one. distinct from `intrinsic`, which names an *LLVM* intrinsic and
        // therefore still produces an llvm::Function - a builtin has no symbol at all, and its
        // call sites fold to a constant. the type it is asking about arrives in instantiation_args
        std::optional<std::string> builtin;

        inline bool is_builtin() const {
            return builtin.has_value();
        }

        FunctionDeclNode() {};
        FunctionDeclNode(TokenReference name_token) :
            name_token(name_token)
        {};

        ~FunctionDeclNode() {};

        inline bool is_anonymous() const {
            return !name_token.has_value();
        }

        inline bool is_generic() const {
            return !type_parameters.empty();
        }

        inline size_t type_parameter_count() const {
            return type_parameters.size();
        }

        const std::string func_name() const {
            if (name_token.has_value()) {
                return name_token.value().value();
            }

            return "[anonymous]";
        }

        // the declared parameter types, in order - the half of the signature overload resolution
        // keys on. a parameter with no resolved type contributes an unknown, which the matcher
        // reads as "says nothing" rather than as a mismatch
        // the declared type of one parameter, unknown when it has none yet. the single spelling of
        // "what type does this parameter have", so a caller comparing one parameter does not have
        // to materialize the whole vector to get the same answer
        inline ValueType parameter_type(size_t index) const {
            return args[index]->has_type() ? args[index]->type() : ValueType::make_unknown();
        }

        inline std::vector<ValueType> parameter_types() const {
            std::vector<ValueType> types;
            types.reserve(args.size());
            for (size_t i = 0; i < args.size(); i++) {
                types.push_back(parameter_type(i));
            }
            return types;
        }

        // the signature as a reader wrote it - `a::foo(int32, float64)`. for diagnostics only;
        // the symbol-table identity is decorated_func_name()
        const std::string signature_description() const;

        // returns the decorated function name as it would appear in the symbol table
        // this is the name that is used to uniquely identify the function aka the mangled name
        const std::string decorated_func_name() const;
        
        const std::string namespaced_func_name() const;

        const std::string get_return_type_description() {
            if (return_type) {
                return return_type->node_description();
            }

            return "void";
        }

        const ValueType get_return_type() const {
            if (return_type) {
                return return_type->type;
            }

            return ValueType::void_type();
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visitFunctionDecl(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif