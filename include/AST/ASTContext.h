#ifndef ASTCONTEXT_H
#define ASTCONTEXT_H

#pragma once

#include "AST/ASTModule.h"
#include "AST/ASTFile.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTDeclarationSite.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTValueType.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace AST
{
    class ClosureExprNode;
    class FunctionDeclNode;
    class TypeNode;
    class TypeDeclNode;
    class VarDeclNode;

    struct Context
    {
        Module &module;

        const TokenizedFile &file;

        // **the namespace every declaration here goes into**, which inside a `{ }` block is that block's
        // *lexical* namespace rather than anything the user wrote - for a type exactly as for a function.
        // deliberately one field and not two: a `namespace a::b;`
        // statement writes this one mid-file, and a parallel "the namespace I really meant" would desync
        // from it silently - every function in a namespaced file would quietly lose its prefix
        Namespace *current_namespace;

        ScopeNode *scope_ptr = nullptr;

        // the function whose body is being parsed, null at file scope. it names the lexical namespaces
        // opened inside that body, so a diagnostic about a block-local declaration can say which
        // function it was written in
        FunctionDeclNode *current_function_ptr = nullptr;

        // innermost-last nest of function-like bodies. a nullptr slot is a named `function` wall:
        // capture through it is refused, because a named function has no environment. ClosureScope
        // pushes; capture_variable walks toward the front. parse-only - after the body both
        // environments already hold the properties, so nothing is stored on the node
        //
        // `current_closure()` is `back()`, which is null inside a named function - including one
        // nested in a closure. that is what turns a read across a function boundary from an error
        // into a capture, and what stops a nested `function` from capturing. carried on the context
        // for the reason self_struct_ptr is: the read site is deep inside the expression parser
        std::vector<ClosureExprNode *> closure_nest {};

        // the closure literal whose body is being parsed, or null inside a named `function`.
        // derived from the nest rather than stored beside it: a named function pushes nullptr, so
        // back() *is* the current closure, including the "there is none" answer
        ClosureExprNode *current_closure() const {
            return closure_nest.empty() ? nullptr : closure_nest.back();
        }

        // the return type of the function body being parsed, null at file scope. this is the
        // destination a `return` fits its expression to, the same way a variable declaration's
        // type is - without it a `return 0` in a `: float64` function typed its literal against
        // nothing and fell back to int32
        TypeNode *return_type_ptr = nullptr;

        // innermost-last stack of the generic type parameters currently in scope. a stack rather
        // than one flat list so a generic member of a generic struct sees both its own parameters
        // and its owner's: lookup walks outward, and leaving an inner scope restores the outer
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

        // and the same thing for a `const function`: `const Foo&`. two nodes rather than one node
        // and a flag because a method's const-ness *is* its receiver's type and nothing else - see
        // AST::receiver_is_const - so the only thing a const method needs is a different args[0],
        // and minting it once per struct body keeps the sharing the mutable one already has
        TypeNode *self_const_type_ptr = nullptr;

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

        // how many loops a `break` or a `continue` written here could leave, innermost outward. zero means
        // there is none, which is the whole of what Parser::parse_loop_control has to check.
        //
        // **a counter and not a stack of loop nodes.** the *target* of an exit is resolved twice further
        // down - by AST::OwnershipPass's frame stack and by CodegenContext::loop_targets - and a third
        // answer recorded here is the one that goes stale when the monomorphizer clones a loop body. a
        // depth is the only part of the question the parser is the right one to answer, and it is also
        // exactly the bound a labelled `break N` would validate against
        unsigned loop_depth = 0;

        // `$__incN` names minted by an increment that binds the target's address once. per file,
        // same reason AST::FixpointLowering's hoist counter is: a golden's `$__inc0` must not move
        // when an unrelated file grows one
        size_t incdec_bind_count = 0;

        // whether a nested declaration parsed here is kept on the file root. a property is walked
        // in both passes so they agree on where it ends, and only the first walk keeps the node -
        // a closure in an instance default would otherwise be add_funcdecl'd from the discarded
        // parse *and* from the clone prepend_property_defaults publishes, two FunctionDeclNodes
        // mangling to one symbol. static initializers are not recipes; they stay true
        bool publish_declarations = true;

        inline ScopeNode &scope() const {
            assert(scope_ptr);
            return *scope_ptr;
        }

        // which of the two receiver nodes a method binds `$this` to. one accessor rather than the
        // pick spelled at each site, so a caller cannot reach for the mutable node while believing
        // it asked for a const method - and so the null-outside-a-struct-body case is one answer
        inline TypeNode *receiver_type(bool is_const) const {
            return is_const ? self_const_type_ptr : self_type_ptr;
        }

        // the scope a *declaration* is emitted into: the file root, whatever block it was written in.
        //
        // a nested `function` is a scoped declaration, not a closure - so its name is block-scoped
        // while the declaration itself is an ordinary top-level one. codegen emits bodies from the file
        // root's children and AST::OwnershipPass resolves drops from the same list, so a declaration
        // left in a body scope is both undefined at link time and never ownership-resolved
        inline ScopeNode &declaration_scope() const {
            ScopeNode *root = &scope();

            while (!root->is_root()) {
                root = root->parent_ptr;
            }

            return *root;
        }

        // push & pop the contexts scope
        void push_scope(ScopeNode &scope);
        void pop_scope();

        // enters a nested type-parameter scope. pushing an empty scope is fine and normal: a
        // non-generic member of a generic owner still needs its own frame so leaving it cannot
        // disturb the owner's parameters
        void push_type_param_scope(const std::vector<TypeParamDecl *> &params) {
            type_param_scopes.push_back(params);
        }

        void pop_type_param_scope() {
            assert(!type_param_scopes.empty());
            type_param_scopes.pop_back();
        }

        // is any generic type parameter reachable from here? deliberately not "is the stack empty":
        // every declaration parser pushes a frame unconditionally, even an empty one, so the stack is
        // never empty inside a body and emptiness would answer this question wrong every time
        bool has_visible_type_params() const {
            return std::any_of(
                type_param_scopes.begin(),
                type_param_scopes.end(),
                [](const std::vector<TypeParamDecl *> &frame) { return !frame.empty(); });
        }

        // the interface whose associated types are in scope, null outside one.
        //
        // **a pointer to the owner rather than a type-parameter frame**, because the list *grows* as
        // the body is read: `type Iter : contract::iterator<V>` has to be resolvable by the requirement written
        // under it, and push_type_param_scope copies the vector, so a frame is a snapshot that cannot
        // learn about a declaration made after it was pushed
        const ComplexType *associated_owner = nullptr;

        // resolves a name against the type parameters in scope, innermost first, so an inner
        // parameter shadows an outer one of the same name. null when the name is not a type
        // parameter here. defined out of line because it reads TypeParamDecl::name
        //
        // the enclosing interface's associated types are consulted *after* the frames, so a type
        // parameter of the same name shadows one - the same innermost-wins rule, one level further out
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
            return CodeRef { &module, file.token_slice };
        }

        CodeRef code_ref(const TokenSlice &slice) const {
            return CodeRef { &module, slice };
        }

        CodeRef code_ref(const TokenReference &tokenref) const {
            if (!module.is_owner_of(tokenref)) {
                throw std::runtime_error("TokenReference does not belong to this module");
            }

            return CodeRef { &module, tokenref.make_slice() };
        }

        /**
         * Creates a virtual token with the same position as the reference token
         *
         * minting belongs to the module, which owns the collection. see Module::make_virtual_token
         */
        TokenReference make_virtual_token(const std::string &value, Token::Type type, const TokenReference &ref) {
            return module.make_virtual_token(value, type, ref);
        }

        /**
         * The suffix that tells two compiler-minted names of the same kind apart: where in the source
         * the thing being named was written, as `<file>L<line>C<column>`.
         *
         * Two names need one - a block's lexical namespace and a closure. A counter is only unique
         * within one *bundle*, and both of these names reach the mangler, so a numbered suffix
         * would make the emitted symbol of a block-local function depend on how many blocks and
         * closures had been parsed before it anywhere in the program. The same declaration in the
         * same library would get a different symbol depending on who else was in the build, which
         * makes a compiled artifact unreusable.
         *
         * Derived from the position rather than numbered because that is the property actually wanted:
         * unique without coordination, and unchanged by an edit somewhere else. The file tag is
         * mandatory - a module tokenizes all of its files into one collection, so line and column alone
         * repeat across files. Two files whose *stem* also matches at the same line and column would
         * still collide, which is loud rather than silent: create_llvm_func_decl refuses two
         * declarations mangling to one name.
         */
        std::string site_discriminator(const TokenReference &at) const;
    };

    // scopes a type-parameter frame to a parser function, so every early return unwinds it.
    // the one guard for all declaration parsers: a hand-rolled copy per parser is how the
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

    // scopes an interface's associated types to its body. saves and restores rather than clearing, for
    // SelfScope's reason: an interface cannot be nested in another, but a *struct* declared inside one
    // would otherwise see its `Iter`
    struct AssociatedTypeScope
    {
        Context &context;
        const ComplexType *previous;

        AssociatedTypeScope(Context &context, const ComplexType *owner) :
            context(context),
            previous(context.associated_owner)
        {
            context.associated_owner = owner;
        }

        AssociatedTypeScope(const AssociatedTypeScope &) = delete;
        AssociatedTypeScope &operator=(const AssociatedTypeScope &) = delete;

        ~AssociatedTypeScope() {
            context.associated_owner = previous;
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
        TypeNode *previous_const_type;

        // both receiver spellings at once: a body that has one has the other, and a frame that set
        // only the mutable node would let `const function` inside it bind `$this` to null
        SelfScope(Context &context, TypeDeclNode *self_struct, TypeNode *self_type,
            TypeNode *self_const_type = nullptr) :
            context(context),
            previous_struct(context.self_struct_ptr),
            previous_type(context.self_type_ptr),
            previous_const_type(context.self_const_type_ptr)
        {
            context.self_struct_ptr = self_struct;
            context.self_type_ptr = self_type;
            context.self_const_type_ptr = self_const_type;
        }

        SelfScope(const SelfScope &) = delete;
        SelfScope &operator=(const SelfScope &) = delete;

        ~SelfScope() {
            context.self_struct_ptr = previous_struct;
            context.self_type_ptr = previous_type;
            context.self_const_type_ptr = previous_const_type;
        }
    };

    // opens a `{ }` block's lexical namespace, so a declaration written inside it belongs to the block
    // rather than to the enclosing namespace - which is the whole of how a nested `function` or a
    // block-local `struct` gets a scope. the namespace is keyed on the block's opening brace, so the
    // declaration pass and the body pass walking the same brace land on one object
    //
    // saves and restores like every guard here. it has to: `namespace a::b;` writes
    // `current_namespace` too, and a block must hand back whatever was current when it opened
    struct LexicalScope
    {
        Context &context;
        Namespace *previous;

        // takes the manager rather than reaching for it, because Context deliberately does not know the
        // collector - every parser that opens a block has `payload.collector.namespaces` at hand
        // `block_token` is the `{` this scope opens at, and it keys the namespace. no block token means
        // no block - only the file root has none - and the guard does nothing at all
        //
        // the one construction, shared by the two walks that open a block: parse_scope in the body pass
        // and parse_declaration_surface in the declaration pass. the two *must* mint the same namespace
        // object for the same brace, so they cannot be allowed to reach retrieve_lexical by two spellings
        // that could drift apart - which is why the display name is derived in here rather than passed
        LexicalScope(
            Context &context,
            NamespaceManager &namespaces,
            const std::optional<TokenReference> &block_token);

        LexicalScope(const LexicalScope &) = delete;
        LexicalScope &operator=(const LexicalScope &) = delete;

        ~LexicalScope() {
            context.current_namespace = previous;
        }
    };

    // opens the namespace a *nested type's declarations* live in: a child of the owner's namespace,
    // named after the owner. `struct A { struct Inner { … } }` puts Inner's constructors and methods in
    // namespace `A`, so `A::Inner(1)` resolves through the ordinary namespace path, a bare `Inner(1)`
    // does not - FunctionRegistry::overloads only ever walks *outward* - and B's `Inner` cannot collide
    // with A's in one overload set.
    //
    // the nested *type* is deliberately not here: it lives on ComplexType::_member_types, the way a
    // method lives on the type rather than in a namespace. that is the split the language already has -
    // Namespace::_symbols holds types only while the FunctionRegistry keys functions by namespace - and
    // it is the same reason the type `A` and the namespace `A` can coexist that lets struct Foo and its
    // constructor Foo already do so
    //
    // saves and restores, for LexicalScope's reason: `namespace a::b;` writes current_namespace too
    struct MemberTypeScope
    {
        Context &context;
        Namespace *previous;

        // takes the manager rather than reaching for it, as LexicalScope does and for the same reason.
        // `owner` is the enclosing type, whose namespace and name together name the child
        MemberTypeScope(Context &context, NamespaceManager &namespaces, const ComplexType &owner);

        MemberTypeScope(const MemberTypeScope &) = delete;
        MemberTypeScope &operator=(const MemberTypeScope &) = delete;

        ~MemberTypeScope() {
            context.current_namespace = previous;
        }
    };

    // scopes the closure literal whose body is being parsed, so a read of an enclosing local inside it is
    // a capture rather than an error. saves and restores, which is what lets a plain `function` nested in
    // a closure body go back to *not* capturing - it is a declaration, not a second closure, and it opens
    // a null frame of this through FunctionBodyScope below
    struct ClosureScope
    {
        Context &context;

        // the closure alone, because the environment parameter is not a second fact: it is `args[0]` of
        // the closure's declaration by construction - push_environment_param puts it there before the
        // parameter list is read, and `is_closure` is what makes implicit_arg_count count it. carrying
        // it separately would be two fields that must agree. a named function pushes nullptr, which
        // is the wall `current_closure()` and through-capture both read
        ClosureScope(Context &context, ClosureExprNode *closure) :
            context(context)
        {
            context.closure_nest.push_back(closure);
        }

        ClosureScope(const ClosureScope &) = delete;
        ClosureScope &operator=(const ClosureScope &) = delete;

        ~ClosureScope() {
            context.closure_nest.pop_back();
        }
    };

    // scopes the function whose body is being parsed, which is what names the lexical namespaces opened
    // inside it. saves and restores rather than clearing, so a declaration nested in a body hands the
    // enclosing function back when it ends
    struct CurrentFunctionScope
    {
        Context &context;
        FunctionDeclNode *previous;

        CurrentFunctionScope(Context &context, FunctionDeclNode *function) :
            context(context), previous(context.current_function_ptr)
        {
            context.current_function_ptr = function;
        }

        CurrentFunctionScope(const CurrentFunctionScope &) = delete;
        CurrentFunctionScope &operator=(const CurrentFunctionScope &) = delete;

        ~CurrentFunctionScope() {
            context.current_function_ptr = previous;
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

    // how many loops the code being parsed sits inside. a loop body enters `previous + 1`;
    // FunctionBodyScope enters 0.
    //
    // it takes a depth rather than incrementing so that one guard serves both askers - and the second one
    // is the load-bearing one: a closure or a nested `function` written inside a loop body does *not*
    // inherit the loop, for the reason every other field in FunctionBodyScope is cleared. without it
    // `while ($c) { $f = function() { break; }; }` parses, and codegen emits a branch to a basic block in
    // a different llvm::Function - which the verifier reports nowhere near the source
    struct LoopScope
    {
        Context &context;
        unsigned previous;

        LoopScope(Context &context, unsigned depth) :
            context(context),
            previous(context.loop_depth)
        {
            context.loop_depth = depth;
        }

        LoopScope(const LoopScope &) = delete;
        LoopScope &operator=(const LoopScope &) = delete;

        ~LoopScope() {
            context.loop_depth = previous;
        }
    };

    // scopes whether nested declarations parsed here land on the file root. saves and restores,
    // like ReturnTypeScope: a property default's discarded re-parse must not leak "do not publish"
    // into a constructor body parsed later in the same walk
    struct DeclarationPublishScope
    {
        Context &context;
        bool previous;

        DeclarationPublishScope(Context &context, bool publish) :
            context(context), previous(context.publish_declarations)
        {
            context.publish_declarations = publish;
        }

        DeclarationPublishScope(const DeclarationPublishScope &) = delete;
        DeclarationPublishScope &operator=(const DeclarationPublishScope &) = delete;

        ~DeclarationPublishScope() {
            context.publish_declarations = previous;
        }
    };

    // every frame a function-like body opens, in one guard: it is no longer inside a struct declaration,
    // no longer inside a constructor, no longer inside a closure unless it *is* one, and it names the
    // lexical namespaces its blocks mint
    //
    // one type rather than four spellings at seven sites, because the four say one thing - "the body of
    // *this* declaration starts here" - and a field left standing at one site is a body reading the
    // enclosing declaration's state. each of them has already been that bug or is one token away from it:
    // without the null self a `function` nested in a method registers as another method of the owner,
    // and without the null closure one nested in a closure body captures - reading that closure's
    // environment parameter out of a different llvm::Function. a per-body field added to Context now has
    // exactly one place it must be cleared
    struct FunctionBodyScope
    {
        SelfScope no_self;
        ConstructorScope no_ctor_this;
        ClosureScope enclosing_closure;
        CurrentFunctionScope current_function;
        LoopScope no_enclosing_loop;

        // the closure defaults to none, because a closure literal's own body is the only one written
        // inside one: a plain `function` nested in a closure body captures nothing, it is a declaration
        FunctionBodyScope(Context &context, FunctionDeclNode *function, ClosureExprNode *closure = nullptr) :
            no_self(context, nullptr, nullptr),
            no_ctor_this(context, nullptr),
            enclosing_closure(context, closure),
            current_function(context, function),
            no_enclosing_loop(context, 0)
        {
        }

        FunctionBodyScope(const FunctionBodyScope &) = delete;
        FunctionBodyScope &operator=(const FunctionBodyScope &) = delete;
    };
};
#endif
