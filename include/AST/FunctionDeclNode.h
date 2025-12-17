#ifndef FUNCTIONDECLNODE_H
#define FUNCTIONDECLNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"

#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/AttributeNode.h"

#include <optional>

namespace AST
{
    class Namespace;
    class AttributeNode;

    // which of the five species a declaration is. spelled out rather than inferred because each is
    // a different shape: a method's `$this` is a borrow parameter, a constructor's is a body-local
    // of value type and its name is the struct's, a destructor takes nothing and returns nothing,
    // an operator's name is a decorated spelling of a symbol nobody can write. it used to be
    // readable off the tokens alone (a constructor is the only thing whose name token differs from
    // its declaration token), which stopped being true the moment a second keyword-declared member
    // existed
    enum class MemberKind
    {
        t_free,
        t_method,
        t_constructor,
        t_destructor,

        // declared `operator (Point $a) + (Point $b) : Point`. a *free* function in every structural
        // sense - null owner_type, registered through FunctionRegistry like a constructor - and this
        // tag is what the two readers that must know it apart read: the mangler, whose name has to
        // survive being a symbol, and the diagnostics, which say "operator" rather than "function"
        t_operator,
    };

    class FunctionDeclNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_func_decl);

        std::optional<TokenReference> name_token;

        // where this declaration is *written*, which is what a module's parse passes reconcile on
        // (AST::DeclarationSite). unset for everything the user spelled with a
        // name, where the name token already is that position
        //
        // a constructor needs the two apart: it is named after its struct, so `name_token` is the
        // struct's name token and every constructor of one struct shares it, while each is declared
        // at its own `constructor` keyword. both are real tokens at a fixed index, so the passes
        // agree without anybody appending a throwaway token to the module to obtain an identity
        std::optional<TokenReference> declaration_token;

        std::vector<VarDeclNode*> args;

        // this function's own generic type parameters (the T, U in `function name<T, U>(...)`),
        // owned by the collector's TypeParamRegistry. a method of a generic struct shares the
        // struct's declarations rather than copying them, so a substitution built from either
        // list binds the same parameters. cleared on a clone, which is concrete by definition
        std::vector<TypeParamDecl *> type_parameters;

        // set when this declaration is a *member* function: the type it was declared inside. a
        // method is an ordinary function whose first parameter is `$this`, so this pointer is the
        // only thing that tells the three consumers that need to know:
        //
        //  - the mangler, which appends an owner segment. without it a method `Foo::get()` and a
        //    free `get(Foo& $f)` mangle identically, and since a method is deliberately absent
        //    from the (namespace, name) overload sets, DuplicateFunctionSignature cannot catch it
        //  - diagnostics, which must not count or render the implicit receiver
        //  - the type parameter prefix, paired with inherited_type_param_count below
        //
        // kept on a clone: an instance is still a member of its owner
        ComplexType *owner_type = nullptr;

        // which species this declaration is. kept on a clone: an instance of a destructor is still
        // a destructor. note it is *not* redundant with owner_type - a constructor is a member of
        // its struct in every sense a reader means, but deliberately has a null owner_type because
        // it resolves through the namespace overload set as a free function named after the struct
        MemberKind member_kind = MemberKind::t_free;

        inline bool is_constructor() const {
            return member_kind == MemberKind::t_constructor;
        }

        inline bool is_destructor() const {
            return member_kind == MemberKind::t_destructor;
        }

        inline bool is_operator() const {
            return member_kind == MemberKind::t_operator;
        }

        // **the symbol an operator declaration was written with**, recovered from its decorated name.
        //
        // an operator's name token holds `AST::operator_function_name`'s answer - "operator +",
        // "operator prefix !!", "operator []" - because the fixity has to be *in* the name for the
        // overload set to tell a prefix from a suffix. a diagnostic wants the symbol back out of it, and
        // this is the one place that reverses the decoration rather than each message slicing the string
        // its own way
        //
        // the empty string for anything that is not an operator, so a caller that forgot to ask
        // is_operator() first gets nothing to print rather than a misleading fragment
        std::string operator_spelling() const {
            if (!is_operator()) {
                return "";
            }

            const std::string decorated = func_name();
            const size_t last_space = decorated.rfind(' ');

            if (last_space == std::string::npos) {
                return decorated;
            }

            return decorated.substr(last_space + 1);
        }

        inline bool is_member() const {
            return owner_type != nullptr;
        }

        // a method declared in an `interface` body: a **requirement**, not an implementation. it is an
        // ordinary member declaration in every other respect - registered through
        // register_member_function, found by find_member_functions, and so callable through a receiver
        // of the interface - but it has no body and no symbol of its own, so nothing may emit or
        // declare one. the three readers are the two loops in TypeLowering::build_function_maps and
        // StmtCodegen::gen_function_decl, exactly the set is_builtin() is skipped in and for the same
        // reason: a `declare` nobody defines fails at link time rather than at the mistake
        //
        // derived from the owner rather than stored as a sixth MemberKind, because a requirement *is*
        // a method - the kind it needs is t_free's method shape, and a second tag would let the two
        // disagree about what the receiver is. defined out of line, ComplexType being incomplete here
        bool is_interface_requirement() const;

        // the body of a `function(...) { ... }` written as an expression. it is an ordinary declaration
        // in every other respect - hoisted to the file root, mangled, emitted - but it is in no overload
        // set, no name reaches it, and its `args[0]` is the environment its captures live in
        bool is_closure = false;

        // how many leading `args` entries the caller did not write: a method's receiver, or a closure's
        // environment. never both, since a closure is not a member - spelled as a count rather than a
        // bool because every consumer wants to offset an index by it
        inline size_t implicit_arg_count() const {
            return (is_member() || is_closure) ? 1 : 0;
        }

        // the callable type a value of this function has. the environment parameter is *not* part of it:
        // it is how a closure reaches its captures, not something a caller passes or a signature
        // promises, exactly as a method's receiver is absent from `signature_description`
        ValueType callable_type() const {
            std::vector<ValueType> params;
            for (size_t i = implicit_arg_count(); i < args.size(); i++) {
                params.push_back(parameter_type(i));
            }
            return ValueType::make_callable(get_return_type(), std::move(params));
        }

        // the 1-based position a reader would count `args[index]` at. the implicit receiver is not
        // something they wrote, so `argument 1` of a method is its first *written* parameter. one
        // accessor rather than `index + 1 - implicit_arg_count()` at each diagnostic, so a second
        // implicit parameter cannot leave some of them off by one
        inline size_t user_arg_number(size_t index) const {
            return index + 1 - implicit_arg_count();
        }

        // how many leading `type_parameters` entries belong to the *owner* rather than to this
        // function. a method of `Box<T>` written `function map<U>(...)` carries [T, U], because
        // one substitution has to bind both - the owner's T from the receiver argument, its own U
        // from the rest - and TypeSubstitution::positional is positional over this whole list.
        //
        // the split matters in two places: an explicit `$b->map<float64>()` spells only the *own*
        // parameters, and signature_description must not render the owner's. cleared on a clone,
        // which is concrete and carries no parameters at all
        size_t inherited_type_param_count = 0;

        inline size_t own_type_param_count() const {
            return type_parameters.size() - inherited_type_param_count;
        }

        // the mirror of ComplexType::template_ref / instantiation_args, for functions: on an
        // instance created by the monomorphizer these name the template it came from and the
        // concrete types it was instantiated with, in declaration order. empty on a template and
        // on a plain non-generic function
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

        // nobody wrote this declaration: the compiler built it from a type's shape. Three producers -
        // the field-wise constructor synthesized by the declaration pass, and the class deinit and copy
        // constructor synthesized by AST::OwnershipPass.
        //
        // it is the *other* half of what AST::function_emission_kind calls generated, beside
        // `is_instantiated()`. Both mean the same thing where it matters: the definition is a pure
        // function of inputs every build shares, so two units may legitimately both need the symbol and
        // it cannot carry external linkage. Two flags rather than one because they are produced in
        // different phases and answer different questions elsewhere - an instance also has a template to
        // point back at, and this has none
        bool is_implicitly_generated = false;

        // `#[inline]`: emit this body into every unit that calls it, rather than once in the unit that
        // declares it. The request is "copy me to the call site's compilation unit", and that is exactly
        // AST::FunctionEmission::t_odr_shared - the same treatment a generic instantiation gets.
        //
        // **it is what keeps cross-module inlining possible without a whole-program merge.** The optimizer
        // can only inline a body it can see; today every unit is linked into one module before the O3
        // pipeline runs, so it sees everything. A build that emits per-module objects instead - which is
        // what an object cache is - hands the optimizer one unit at a time, and then a callee in another
        // module is just a `declare`. Marking it copies the body across, so the inliner has something to
        // work with either way.
        //
        // the ODR obligation that comes with t_odr_shared applies: two copies of this body must be
        // identical, so nothing about it may depend on which unit it was emitted into.
        //
        // deliberately *not* a promise the optimizer has to keep. It is a placement instruction, not
        // `always_inline` - naming it after the outcome rather than the mechanism is what lets the
        // attribute stay honest when the inliner declines.
        bool is_inline = false;

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

        // for a declaration whose name is written somewhere other than where it is declared - a
        // constructor, which is named after its struct. TokenReference holds a reference, so the
        // members are not assignable after the fact
        FunctionDeclNode(TokenReference name_token, TokenReference declaration_token) :
            name_token(name_token),
            declaration_token(declaration_token)
        {};

        ~FunctionDeclNode() {};

        inline bool is_anonymous() const {
            return !name_token.has_value();
        }

        // the token that identifies this declaration: where it is written, which is its own
        // `declaration_token` when it has one and its name token otherwise. only valid when the
        // declaration is not anonymous
        inline const TokenReference &declaration_site_token() const {
            return declaration_token.has_value() ? declaration_token.value() : name_token.value();
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
