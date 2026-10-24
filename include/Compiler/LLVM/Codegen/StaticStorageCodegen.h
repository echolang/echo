#ifndef STATICSTORAGECODEGEN_H
#define STATICSTORAGECODEGEN_H

#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>

#include <string>

namespace AST
{
    class FunctionDeclNode;
    class StaticPropertyExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // static property storage. lazy first-use through a self-guarding init (`llvm.global_ctors`
    // is a no-op under `echoc run`). teardown is LIFO from `main`'s epilogue (`atexit` unmaps
    // under the JIT, and runs after the allocation report). `die` / `assert` / `exit` skip it
    class StaticStorageCodegen
    {
    public:
        StaticStorageCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // the address of `node`'s storage, with its initializer already run.
        //
        // **one `call` to the init function, never an inline branch.** `gen_lvalue` is a pure address
        // producer, and an inline `if (!guard)` would make it create basic blocks - which drags
        // `set_insert_point`'s sticky-debug-location trap into the hottest shared path in codegen.
        // `dprint` is currently the only builtin whose lowering creates blocks and it got its own
        // subsystem for exactly that reason. the per-unit O2 pipeline inlines the call anyway, so
        // `--optimize none` gets one legible call an IR golden can assert on and `-O` gets the
        // load-and-branch that would have been written by hand
        llvm::Value *gen_address(AST::StaticPropertyExprNode &node);

        // the teardown walk, emitted into `main`'s epilogue. a no-op in a program that declares no
        // static, since nothing ever pushed a node onto the chain
        void gen_teardown();

        // the `linkonce_odr` walker over the teardown chain. `create_head` is a
        // native lib's fini: this unit may reference no static of its own, and
        // still has to walk a chain a dependency pushed. an executable passes
        // false so a program with no static still emits no walk
        llvm::Function *ensure_teardown_walker(bool create_head);

    private:
        CodegenContext &_ctx;

        // `<owner mangled token>.s<index>.<name>` - the storage, zero-initialized and `linkonce_odr`.
        // the owner's mangled token carries its type arguments, so `Box<int32>` and `Box<float>` name
        // two globals with no extra work. the *index* is in there beside the name for the reason a
        // struct field's is: a rename is a different symbol and a reorder is not
        llvm::GlobalVariable *storage_for(AST::StaticPropertyExprNode &node, const std::string &symbol);

        // the word beside it: 0, a tid token, or ~0. a word rather than an i8 so the in-progress
        // state can name its owner, which is what keeps the documented recursion answer from
        // becoming a pthread_once deadlock. a tid that is 0 or ~0 is stored as 1 so it cannot
        // look uninitialized or finished
        llvm::GlobalVariable *guard_for(const std::string &symbol);

        // the slow path: CAS 0 → self, run the body, store ~0. one function for the
        // program, linkonce_odr. a row without OS threads is a load/store instead.
        // the fast path stays an acquire load in `<sym>.init` when there are threads
        llvm::Function *once_helper();

        // the self-guarding init function, emitted on first reference in this unit. **the guard is
        // stored before the body runs**, which is also the recursion answer: a static whose initializer
        // reads itself re-enters, finds the guard set, and reads the zero the global was created with.
        // defined rather than undefined, because `get_or_create_odr_global` zero-initializes
        llvm::Function *init_for(AST::StaticPropertyExprNode &node, const std::string &symbol);

        // the symbol both globals and the function are named from. **taken once per access and handed
        // down**, rather than re-derived by each of the four: it walks the owner's `mangled_token()`,
        // which is recursive through an instantiation's arguments and allocates a segment vector on
        // the way, and this runs per access node
        std::string symbol_for(AST::StaticPropertyExprNode &node);

        // **the callee, declared into this unit on demand.** TypeLowering::build_function_maps declares
        // a symbol into a unit either because the unit holds the declaration or because something in
        // its *tree* references it - and nothing in the tree references an initializer or a teardown:
        // the only call to either is the one being emitted right here. so the unit is asked to declare
        // it, which is idempotent and is what the reference-scoped sweep would have done had there been
        // a reference to find. null when there is nothing to call
        llvm::Function *declare_on_demand(AST::FunctionDeclNode *decl);
    };
};

#endif
