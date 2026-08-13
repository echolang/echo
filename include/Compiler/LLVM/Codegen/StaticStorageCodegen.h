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

    // **the one owner of where a static property's storage comes from, when it is filled, and when it
    // is torn down.** a sibling of ProcessCodegen and MemoryCodegen, and an *emitted* runtime like
    // both: everything here is `linkonce_odr` in whichever unit references it, so a module's object
    // never depends on its consumers.
    //
    // three facts, and each of them is a language rule rather than an implementation detail - they are
    // stated in docs/language/ for that reason:
    //
    // **initialization is lazy and first-use.** every read and every write goes through a call to a
    // self-guarding init function, which runs the initializer once. the alternative was
    // `llvm.global_ctors`, and it is not merely worse here - `Backend::run_code` never calls
    // `runStaticConstructorsDestructors`, so `echoc build` would initialize statics and `echoc run`
    // would silently not. the corpus is overwhelmingly `mode: run`, so that divergence would have been
    // pinned as golden. the consequence to document is that a static nothing ever names is never
    // initialized, so its initializer's side effects never run
    //
    // **teardown is reverse-of-initialization, from `main`'s epilogue.** the init function pushes a
    // node onto an intrusive chain after seating the value, so the walk is LIFO for free. `atexit`
    // was the obvious alternative and is a use-after-free here: under `echoc run` the JIT'd program
    // would register handlers, and `~LLVMCompiler` deletes the execution engine - unmapping the code -
    // while `LLVMCompiler` is still a stack local of `main_run`. it would also run *after*
    // `MemoryCodegen::gen_report`, so every owning static would read as a leak under
    // `--track-allocations`, which every corpus case passes
    //
    // **`die`, a failed `assert` and `std::env::exit` skip teardown**, because they leave `main`'s
    // block terminated. that is the same thing module-scope releases already do, so it is not a new
    // rule to learn
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

    private:
        CodegenContext &_ctx;

        // `<owner mangled token>.s<index>.<name>` - the storage, zero-initialized and `linkonce_odr`.
        // the owner's mangled token carries its type arguments, so `Box<int32>` and `Box<float>` name
        // two globals with no extra work. the *index* is in there beside the name for the reason a
        // struct field's is: a rename is a different symbol and a reorder is not
        llvm::GlobalVariable *storage_for(AST::StaticPropertyExprNode &node, const std::string &symbol);

        // the `i8` guard beside it. i8 rather than i1 so it reads as the byte the store actually is,
        // which is what an IR golden shows
        llvm::GlobalVariable *guard_for(const std::string &symbol);

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
