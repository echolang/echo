#ifndef CODEGENCONTEXT_H
#define CODEGENCONTEXT_H

#pragma once

#include "eco.h"
#include "Compiler/CompilerException.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "AST/ASTCoreTypes.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <cassert>
#include <memory>
#include <optional>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

namespace AST
{
    class File;
    class Visitor;
    class VarDeclNode;
    class FunctionDeclNode;
};

namespace Compiler::LLVM
{
    class TypeLowering;
    class LValueCodegen;
    class ClassCodegen;
    class AbortCodegen;
    class MemoryCodegen;
    class ProcessCodegen;
    class DebugPrintCodegen;

    // shared mutable state threaded through every codegen subsystem. owns the llvm context and
    // builder, the per-module compilation units, and the transient value/variable bookkeeping the
    // visitor recursion relies on. the subsystems (TypeLowering, ExprCodegen, StmtCodegen,
    // TypeDeclCodegen, Backend) all hold a reference to one of these and talk to the shared state
    // exclusively through it
    struct CodegenContext
    {
        std::unique_ptr<llvm::LLVMContext> llvm_context;
        std::unique_ptr<llvm::IRBuilder<>> builder;

        // what the invocation asked for. read through its predicates rather than compared, so that
        // every check the compiler can skip skips together
        CompilerOptions options;

        // the stdlib types the compiler names, bound by `#[core: "..."]` during parsing. published here
        // by compile_bundle rather than reached for, so codegen never holds the whole collector - the
        // only thing it needs from it is which declared type is `string`
        const AST::CoreTypes *core_types_ptr = nullptr;

        const AST::CoreTypes &core_types() const {
            assert(core_types_ptr && "core types not published - compile_bundle must set them");
            return *core_types_ptr;
        }

        // where the fields of the bound `string` and `string::view` sit, resolved **once** beside the
        // binding above rather than per literal: it is a fact about the program, not about the node
        // being lowered, and the diagnostic for a malformed stdlib string should fire once whether or
        // not some literal happens to reach codegen. nullopt when no stdlib declared one at all
        std::optional<AST::CoreStringLayout> string_layout;

        const AST::CoreStringLayout &core_string_layout() const {
            assert(string_layout.has_value() && "string layout not resolved - compile_bundle must resolve it");
            return string_layout.value();
        }

        // the two words a string is read as: the bytes and how many of them.
        //
        // **the other half of what PrintfConversion.h was extracted for.** the same two printers ask -
        // `echo`, one scalar per statement, and the `dprint` builtin, a whole value's structure - and
        // getting the *window* out is as much a shared fact as which conversion specifier to use: a
        // `string` wraps a `view` and is one level further out than it, a `view` is the window itself, and
        // both are resolved by index off the layout the core binding published, never by position (see
        // AST::resolve_core_string_layout)
        //
        // takes a value rather than an address: both askers already hold one, and a substring shares its
        // owner's buffer, so there is nothing here to load through
        struct StringWindow
        {
            llvm::Value *bytes;
            llvm::Value *size;
        };

        StringWindow gen_string_window(llvm::Value *value, const AST::ValueType &type, const char *prefix);

        // the registry an interface **widening** needs, published here by compile_bundle for the reason
        // core_types_ptr above is - so codegen still never holds the whole collector.
        //
        // why codegen needs it at all: an erased value carries its vtable, and the vtable is resolved
        // where the concrete class is still known, which is the widening site. filling it means asking
        // AST::interface_implementations which declaration answers each requirement, and matching a
        // *generic* interface's requirement (`Comparable<Money>`'s `T`) against its implementor
        // re-substitutes that type. every such lookup is a cache hit by now - the type checker resolved
        // the same conformance before codegen ran - so this interns nothing new
        AST::TypeRegistry *type_registry_ptr = nullptr;

        AST::TypeRegistry &type_registry() const {
            assert(type_registry_ptr && "type registry not published - compile_bundle must set it");
            return *type_registry_ptr;
        }

        // the host target's data layout and triple, published by Backend::init_target before any
        // module is created so that every module carries them from the start. this is what makes
        // a compile-time `size_of<T>()` answer the same number the running program will see -
        // asking a layout-less module gives LLVM's defaults, which match no real target
        std::optional<llvm::DataLayout> data_layout;
        std::string target_triple;

        const llvm::DataLayout &layout() const {
            assert(data_layout.has_value() && "target not initialized - Backend::init_target must run before codegen");
            return data_layout.value();
        }

        std::vector<std::unique_ptr<CmpUnit>> cmp_units;
        std::unordered_map<std::string, CmpUnit *> cmp_unit_map;

        CmpUnit *current_cmp_unit = nullptr;
        AST::File *current_file = nullptr;

        // the file each function declaration was written in, so a body's own source position does not
        // depend on which walk reached it.
        //
        // it exists because a body's *content* can read `current_file`: AbortCodegen::location_of folds
        // `<file>:<line>` into the private string an `assert` or a `die` aborts with. A body emitted from
        // its own file root got the right answer by luck of the walk, and the moment a definition is
        // emitted into a unit other than the one owning its declaration - which is what a generic
        // instantiation shared by two units needs - the same linkonce_odr symbol would carry two different
        // messages and the linker would keep an arbitrary one.
        //
        // a lookup miss is legitimate and falls back to the ambient file: a declaration reached other than
        // through a file root has no better answer available, and the fallback is exactly today's
        // behaviour
        std::unordered_map<const AST::FunctionDeclNode *, AST::File *> function_file_map;

        AST::File *file_of(const AST::FunctionDeclNode *decl) const {
            auto found = function_file_map.find(decl);
            return found != function_file_map.end() ? found->second : current_file;
        }

        // the function declaration currently being generated, set/restored around each function
        // body so codegen errors can name their enclosing function. null at global scope
        AST::FunctionDeclNode *current_function = nullptr;

        std::stack<llvm::Value *> value_stack;
        std::unordered_map<AST::VarDeclNode *, llvm::AllocaInst *> var_map;

        // **the slot each `?->` currently being lowered spilled its unwrapped base into**, innermost last.
        // an AST::ChainBaseNode names the top one - it is the marker standing for that base inside the
        // chain's continuation, and a chain nested in another's continuation pushes and pops around its own
        //
        // here rather than on ExprCodegen because two subsystems read it: the expression arm, for the
        // marker in value position, and LValueCodegen, for a method receiver or a write through the chain.
        // the slot *borrows* - nothing is retained into it and nothing dropped out, exactly as a method's
        // `$this` borrows what it was handed
        std::vector<llvm::Value *> chain_base_slots;

        // **where a `break` and a `continue` go**, innermost last.
        //
        // two blocks per loop rather than the loop's AST node, because the two are different edges - and
        // because that is the only thing a C-style `for` would need: its step block goes in
        // `continue_block` and nothing else here changes. for a `while` the continue target is the
        // condition block, since the condition *is* the step
        //
        // here rather than on StmtCodegen for chain_base_slots' reason: the loop pushes it and the exit
        // reads it, and one owner is what keeps the two from disagreeing about which loop is innermost
        struct LoopTarget
        {
            llvm::BasicBlock *break_block = nullptr;
            llvm::BasicBlock *continue_block = nullptr;
        };

        std::vector<LoopTarget> loop_targets;

        // the owning LLVMCompiler, so subsystems can recurse into child nodes through the single
        // AST::Visitor that the node accept() dispatch requires.
        AST::Visitor *visitor = nullptr;

        // the type-lowering subsystem, reachable from any subsystem that needs to map an
        // AST::ValueType to an llvm::Type.
        TypeLowering *types = nullptr;

        // the lvalue subsystem: the single place that turns an expression into an address
        // every read, write and address-of goes through it, so they cannot drift apart
        LValueCodegen *lvalues = nullptr;

        // the class subsystem: allocation and the two reference-count operations. reachable from the
        // statement and expression subsystems, which is where the tree says a retain or a release goes
        ClassCodegen *classes = nullptr;

        // the abort subsystem: the one owner of how a program stops. every stop site - `die`, a
        // failed `assert`, the null narrowing check - goes through it, so they share one runtime,
        // one message shape and one release-mode gate
        AbortCodegen *abort = nullptr;

        // the memory subsystem: the one owner of where heap memory comes from. the class subsystem's
        // boxes and environments and the stdlib's raw buffers both go through it, which is what makes
        // "how much is still outstanding" a question with an answer at all
        MemoryCodegen *memory = nullptr;

        // the process subsystem: the one owner of where a program's arguments and environment come
        // from. the entry point fills it in and the three `process_*` builtins read it back, which is
        // the whole of how anything the platform hands `main` reaches Echo
        ProcessCodegen *process = nullptr;

        // the debug-print subsystem: the whole of how `dprint` renders a value. reachable from the
        // expression subsystem, which is where the builtin's call site is - and its own subsystem
        // because it carries state across a recursion and creates basic blocks, neither of which an
        // expression arm may do
        DebugPrintCodegen *debug_print = nullptr;

        llvm::Module *current_module() {
            return current_cmp_unit->llvm_module.get();
        }

        // declares a libc function into the current module, or hands back the one already there
        //
        // the RC runtime, the abort runtime and `echo` are all *emitted* rather than linked, so
        // each needs a handful of C symbols with no stdlib declaration behind them. spelled out per
        // symbol they had already drifted - `printf` still uses the legacy typed pointer, and only
        // `exit` remembered its attributes - so this is the one spelling
        llvm::FunctionCallee libc_callee(
            const char *name,
            llvm::Type *return_type,
            llvm::ArrayRef<llvm::Type *> parameter_types,
            bool variadic = false)
        {
            return current_module()->getOrInsertFunction(
                name, llvm::FunctionType::get(return_type, parameter_types, variadic));
        }

        // the opaque `ptr`, spelled once - it appears in almost every emitted runtime signature
        llvm::Type *opaque_ptr_type() const {
            return llvm::PointerType::get(*llvm_context, 0);
        }

        // a **zero-initialized linkonce_odr global** in the current module, or the one already there.
        //
        // the emitted runtime's state lives in these - the allocation counter and the three process
        // globals - and every one of them wants the same three properties for the same reason the runtime
        // functions beside them do: emitted per unit rather than linked, folded by the linker, and
        // starting at zero so a unit whose `main` never ran the capture reads a defined value rather than
        // whatever was there. spelled per subsystem they were two identical copies of this
        llvm::GlobalVariable *get_or_create_odr_global(const char *symbol, llvm::Type *type) {
            if (auto *existing = current_module()->getGlobalVariable(symbol, true)) {
                return existing;
            }

            return new llvm::GlobalVariable(
                *current_module(),
                type,
                /*isConstant=*/false,
                llvm::GlobalValue::LinkOnceODRLinkage,
                llvm::Constant::getNullValue(type),
                symbol);
        }

        // **has the block being emitted into already ended?** one question with many askers, and
        // the answer to all of them is "then emit nothing more here": gen_scope stops walking a
        // scope, gen_function_decl declines to synthesize a second terminator, the if/while arms
        // decline to branch, and compile_bundle's main epilogue declines to return
        //
        // named because a second terminator in one block fails the verifier, so every emitter that
        // can follow a `return`, a `die` or a `break` owes this check
        bool block_is_terminated() const {
            llvm::BasicBlock *block = builder->GetInsertBlock();
            return block != nullptr && block->getTerminator() != nullptr;
        }

        // **every stack slot in the language comes from here.**
        //
        // an `alloca` is an instruction, not a declaration, so it re-runs every time control reaches it -
        // and the builder stands wherever the emitter that asked happens to be. a local declared in a loop
        // body was therefore allocated once per iteration, growing the stack until the function returned,
        // with no `llvm.stackrestore` anywhere to give it back. `-O` did not take it back either: mem2reg
        // and SROA only promote allocas they find in the entry block, so the loop local nobody could
        // promote was also the one most worth promoting
        //
        // the entry block runs exactly once per call, which is the lifetime a slot actually wants. it is
        // also where LLVM's own frontends put theirs, and what the whole "static alloca" contract is
        //
        // **only the slot travels.** an initializing or zeroing store is a statement and stays where it
        // was written - see StmtCodegen::ensure_var_slot, whose zero-init has to re-run once per turn of a
        // loop for a `Foo $x;` to be re-cleared. hoisting the two together is the mistake this splits
        llvm::AllocaInst *entry_alloca(llvm::Type *type, const llvm::Twine &name) {
            llvm::BasicBlock &entry = builder->GetInsertBlock()->getParent()->getEntryBlock();

            // **after the slots already there, not in front of them.** getFirstInsertionPt alone would put
            // each new one at the top, so a function's parameters came out in reverse and every IR golden
            // had to be rewritten for no reason. past the run instead keeps the allocas contiguous and in
            // the order they were asked for, which is what the emitters here have always produced -
            // getFirstNonPHIOrDbgOrAlloca is exactly that point, and it skips *static* allocas only, which
            // is all of them because this is the one place they are minted and it never passes an array size
            //
            // its own builder rather than a save/restore of the shared one: this is called from the middle
            // of emitting something else, and an early return between the two would leave the shared
            // insert point stranded in the entry block
            llvm::IRBuilder<> at_entry(&entry, entry.getFirstNonPHIOrDbgOrAlloca());

            return at_entry.CreateAlloca(type, nullptr, name);
        }

        void push(llvm::Value *value) {
            value_stack.push(value);
        }

        llvm::Value *pop() {
            auto value = value_stack.top();
            value_stack.pop();
            return value;
        }

        // the module whose file-scope statements become the C `main`. Defaults to ECO_MAIN_MODULE_NAME,
        // which is what loose sources on the command line are collected into.
        //
        // it is a *name* rather than the constant because a project is not a pile of files: `echoc run` in a
        // directory holding a module.eco compiles that manifest, and a manifest calls its module whatever
        // the project is called. Requiring `#[module: "main"]` to make a project runnable would be naming
        // the compiler's internals in every user's manifest
        std::string entry_module_name = ECO_MAIN_MODULE_NAME;

        // the main compilation unit, or nullptr if the bundle has no main module yet
        CmpUnit *main_cmp_unit();

        std::string llvm_err_str();

        // a human-readable description of the current codegen location, e.g. "in function 'foo'"
        // or "at global scope", suffixed onto codegen error messages
        std::string function_context() const;

        // the name of the file being emitted. the *file name*, not the path: the e2e runner passes
        // an absolute source directory, so a path would make every golden machine-specific
        //
        // beside function_context() rather than on a subsystem, because it answers the same kind of
        // question from the same state, and the one caller that needs both would otherwise reach
        // through two different owners for one sentence of output
        std::string current_file_name() const;

        Compiler::InternalCompilerException error(std::string message);
    };

    // pushes a loop's two exit targets for the duration of its **body**, the codegen mirror of
    // AST::LoopScope. it must never wrap a loop's condition: a `break` written in one belongs to an
    // enclosing loop, and a stack pushed too early sends it to the wrong merge block - valid IR, wrong
    // program, and nothing downstream can tell
    struct LoopTargetScope
    {
        CodegenContext &ctx;

        LoopTargetScope(CodegenContext &ctx, llvm::BasicBlock *break_block, llvm::BasicBlock *continue_block)
            : ctx(ctx)
        {
            ctx.loop_targets.push_back(CodegenContext::LoopTarget{break_block, continue_block});
        }

        LoopTargetScope(const LoopTargetScope &) = delete;
        LoopTargetScope &operator=(const LoopTargetScope &) = delete;

        ~LoopTargetScope() {
            ctx.loop_targets.pop_back();
        }
    };
};

#endif
