#ifndef CODEGENCONTEXT_H
#define CODEGENCONTEXT_H

#pragma once

#include "eco.h"
#include "Compiler/CompilerException.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/LLVM/Codegen/ReturnAbi.h"
#include "Compiler/LLVM/Codegen/TbaaTree.h"
#include "AST/ASTCoreTypes.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/TargetParser/Triple.h>

#include <cassert>
#include <filesystem>
#include <memory>
#include <optional>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Function;
};

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
    class AtomicCodegen;
    class MemoryCodegen;
    class StaticStorageCodegen;
    class ProcessCodegen;
    class DebugPrintCodegen;
    class ErasureCodegen;
    class DebugInfoCodegen;

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

        // the stdlib types the compiler names, bound by `#[core: ...]` during parsing. published here
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

        // the same fact for `crash::info`: resolved once beside the binding, not per abort.
        // nullopt when nothing declared one - `--no-stdlib`, and a program that never bound
        // `#[core: crash_info]`. the abort thunk is still piece-wise then; only hook *dispatch*
        // needs this
        std::optional<AST::CoreCrashInfoLayout> crash_info_layout;

        const AST::CoreCrashInfoLayout &core_crash_info_layout() const {
            assert(crash_info_layout.has_value() && "crash info layout not resolved - compile_bundle must resolve it");
            return crash_info_layout.value();
        }

        // the two words a string is read as: the bytes and how many of them.
        //
        // the value is a `string::view`. a `string` becomes one first, through
        // `string_as_view`, so this never has to know how the live bytes are stored.
        // resolved by index off the layout the core binding published, never by
        // position (see AST::resolve_core_string_layout)
        //
        // takes a value rather than an address: both askers already hold one, and a substring shares its
        // owner's buffer, so there is nothing here to load through
        struct StringWindow
        {
            llvm::Value *bytes;
            llvm::Value *size;
        };

        StringWindow gen_string_window(llvm::Value *view, const char *prefix);

        // a `string` becomes a `view` through the conversion the type already
        // declared. a view is handed back as-is. a hand-declared `#[core: string]`
        // with no conversion still has a live window, and that is the fallback
        llvm::Value *string_as_view(
            llvm::Value *value,
            const AST::ValueType &type,
            const char *prefix);

        // **the caller's half of the return ABI, and the only place a call to an Echo function is
        // emitted.** echo, die and dprint convert a `string` through this, and every ordinary
        // call does too - one dance, because a site that allocated the slot and forgot the
        // attribute is a *miscompile*
        void emit_call(
            llvm::FunctionCallee callee,
            std::vector<llvm::Value *> &args,
            const ReturnAbi &abi);

        // the llvm::Function a declaration was emitted as, declared into this unit
        // on demand. null when nothing was emitted for it; the caller phrases the diagnostic
        llvm::Function *llvm_function(const AST::FunctionDeclNode *decl);

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
        // a body's *content* no longer reads this: the abort path asks the call's own token.
        // the map remains for the declaration-site question A40 will retire onto
        // DeclarationOrigin - an ODR-shared body's DISubprogram file must not depend on which walk
        // reached it.
        //
        // a lookup miss is legitimate and falls back to the ambient file: a declaration reached other
        // than through a file root has no better answer available
        std::unordered_map<const AST::FunctionDeclNode *, AST::File *> function_file_map;

        AST::File *file_of(const AST::FunctionDeclNode *decl) const {
            auto found = function_file_map.find(decl);
            return found != function_file_map.end() ? found->second : current_file;
        }

        // a token names its own file (`TokenReference::file`) and whether this compiler minted it
        // (`TokenReference::is_minted`). there is no bundle-wide scan and no ambient fallback for
        // either question

        // **and where each declared type was written**, for the map above's reason rather than as a
        // convenience: a type's description is emitted into every unit that mentions it, so anything in
        // it taken from the ambient unit makes two descriptions of one type. That is not hypothetical -
        // a struct's DIType named the first file of whichever module was being lowered, so `map<K,V>`
        // was declared in `arr.eco` in one object and in the user's file in the next, and
        // verify_odr_consistency refused the build.
        //
        // an instantiation has no declaration node of its own, so a caller reads through
        // ComplexType::template_or_self() exactly as the function map's third sweep does
        struct TypeSite
        {
            AST::File *file = nullptr;
            uint32_t line = 0;
        };

        std::unordered_map<const AST::ComplexType *, TypeSite> type_site_map;

        std::optional<TypeSite> site_of(const AST::ComplexType *type) const {
            if (type == nullptr) {
                return std::nullopt;
            }

            auto found = type_site_map.find(type->template_or_self());

            return found != type_site_map.end() ? std::optional<TypeSite>(found->second) : std::nullopt;
        }

        // the function declaration currently being generated, set/restored around each function
        // body so codegen errors can name their enclosing function. null at global scope
        AST::FunctionDeclNode *current_function = nullptr;

        // **where this function writes its answer, when the answer comes back through storage.** null for
        // every function whose return fits in registers, which is the whole of the condition -
        // Compiler::LLVM::return_abi_for owns it and each of the four sites asks that rather than deciding.
        //
        // set and restored around each body like current_function, a body being able to hold another's
        llvm::Value *sret_pointer = nullptr;
        llvm::Type *sret_type = nullptr;

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

        // **the type-based alias tree**, minted once beside the llvm context it hangs its nodes off.
        // one tree and not one per unit, deliberately: MDNodes are owned by the context, so two units
        // sharing it must share the leaves too or an `int32` access in one would carry a node that
        // is merely *equal* to the other's rather than the same, and LLVM compares them by pointer
        //
        // see TbaaTree.h for what may be tagged and, more importantly, what may not
        std::unique_ptr<TbaaTree> tbaa;

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

        // the seven `mem::atomic::` verbs. one protocol, one file - see AtomicCodegen.h
        AtomicCodegen *atomics = nullptr;

        // the memory subsystem: the one owner of where heap memory comes from. the class subsystem's
        // boxes and environments and the stdlib's raw buffers both go through it, which is what makes
        // "how much is still outstanding" a question with an answer at all
        MemoryCodegen *memory = nullptr;

        // the process subsystem: the one owner of where a program's arguments and environment come
        // from. the entry point fills it in and the three `process_*` builtins read it back, which is
        // the whole of how anything the platform hands `main` reaches Echo
        ProcessCodegen *process = nullptr;

        // the static-storage subsystem: the one owner of where a static property's storage comes
        // from, when it is filled and when it is torn down. reachable from the lvalue subsystem,
        // which is where every access to one goes through
        StaticStorageCodegen *statics = nullptr;

        // the debug-print subsystem: the whole of how `dprint` renders a value. reachable from the
        // expression subsystem, which is where the builtin's call site is - and its own subsystem
        // because it carries state across a recursion and creates basic blocks, neither of which an
        // expression arm may do
        DebugPrintCodegen *debug_print = nullptr;

        // owning class-handle erasure: `erased::from`, retain/release, and `assume`. reachable from
        // the expression subsystem, which is where those builtins are called
        ErasureCodegen *erasure = nullptr;

        // the debug-info subsystem: the whole of what a debugger is told. every entry point on it is a
        // no-op with `-g` off, which is what keeps the call sites free of a flag check - and it is its
        // own subsystem for AbortCodegen's reason, being state carried across a whole function body
        // rather than a fact about the node in hand
        DebugInfoCodegen *debug_info = nullptr;

        // `__eco_static_once` names pthread_self / sched_yield. set when that helper is
        // emitted, read by the driver so the compiler-introduced symbols carry their own
        // link requirement rather than borrowing the stdlib's
        bool needs_pthread = false;

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

        // the object is for this machine, so CRT names follow the triple rather than
        // `--target-os`. that flag only picks `#[if:]` arms; it does not retarget
        bool targeting_windows() const {
            return llvm::Triple(target_triple).isOSWindows();
        }

        // `write(fd, ptr, len)` on POSIX, `_write` on Windows UCRT. length is i64 in
        // IR either way; Windows truncates to i32 because that is the CRT's count
        void emit_libc_write(int fd, llvm::Value *ptr, llvm::Value *len)
        {
            llvm::Type *i32 = llvm::Type::getInt32Ty(*llvm_context);
            llvm::Type *i64 = llvm::Type::getInt64Ty(*llvm_context);
            llvm::Type *opaque_ptr = opaque_ptr_type();
            llvm::Value *fd_val = llvm::ConstantInt::get(i32, fd);

            if (targeting_windows()) {
                llvm::Value *count = builder->CreateTrunc(len, i32, "write.n");
                builder->CreateCall(
                    libc_callee("_write", i32, { i32, opaque_ptr, i32 }),
                    { fd_val, ptr, count });
                return;
            }

            builder->CreateCall(
                libc_callee("write", i64, { i32, opaque_ptr, i64 }),
                { fd_val, ptr, len });
        }

        // stdout is fully buffered when it is a pipe. `echo` of a string goes through
        // `_write` (unbuffered) after `fflush(NULL)`, and that fflush is a no-op from
        // JIT'd code on Windows: MCJIT resolves UCRT but `fflush(NULL)` does not drain
        // this process's FILE*. unbuffering stdout and stderr makes `printf` and
        // `_write` the same kind of write, so program order is what the goldens record
        void emit_unbuffer_stdio()
        {
            if (!targeting_windows()) {
                return;
            }

            llvm::Type *i32 = llvm::Type::getInt32Ty(*llvm_context);
            llvm::Type *i64 = llvm::Type::getInt64Ty(*llvm_context);
            llvm::Type *ptr = opaque_ptr_type();
            llvm::FunctionCallee iob = libc_callee("__acrt_iob_func", ptr, { i32 });
            llvm::FunctionCallee setvbuf_fn = libc_callee(
                "setvbuf", i32, { ptr, ptr, i32, i64 });
            llvm::Value *null = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptr));
            llvm::Value *ionbf = llvm::ConstantInt::get(i32, 4);
            llvm::Value *zero = llvm::ConstantInt::get(i64, 0);

            for (unsigned fd : { 1u, 2u }) {
                llvm::Value *file = builder->CreateCall(
                    iob, { llvm::ConstantInt::get(i32, fd) });
                builder->CreateCall(setvbuf_fn, { file, null, ionbf, zero });
            }
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

            auto *global = new llvm::GlobalVariable(
                *current_module(),
                type,
                /*isConstant=*/false,
                llvm::GlobalValue::LinkOnceODRLinkage,
                llvm::Constant::getNullValue(type),
                symbol);

            // an atomic access to a global with unstated alignment is a verifier error at best
            // and two units disagreeing about one linkonce_odr symbol at worst
            global->setAlignment(layout().getABITypeAlign(type));
            return global;
        }

        // **where the builder goes, and the one place it goes there.**
        //
        // an IRBuilder's debug location is *sticky* across SetInsertPoint. Inside a statement that is
        // exactly right - every block an `if` or a `while` creates belongs to the statement that wrote
        // it - but a **merge block created after one carries a location from inside an arm**, and the
        // block a following statement lands in carries the previous statement's until something sets
        // it. Neither is a verifier error and neither shows up in a dump: the failure is a debugger
        // stepping to a line the program is not on.
        //
        // so the location travels with the move, and there is no second spelling of moving the builder.
        // beside entry_alloca for its reason - the alternative is a rule that fourteen call sites have
        // to remember, which is the shape gen_load and gen_store already exist to avoid
        void set_insert_point(llvm::BasicBlock *block);

        // the restoring form, for an emitter that stepped out of a body to build a thunk and is putting
        // the builder back where it found it. Two spellings of the move would be two answers to whether
        // the location travels with it, which is the one thing this exists to settle
        void set_insert_point(llvm::BasicBlock *block, llvm::BasicBlock::iterator point);

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

        // the one file of that module whose root is the program, when a target named one.
        //
        // **empty means every file root of the entry module is**, which is what loose sources on the
        // command line and a manifest declaring no target both get - and what every program compiled
        // before targets existed got. A target narrows it to one file, so a module holding two entries
        // produces two programs rather than one concatenation of both in filename order.
        //
        // a path rather than an AST::File*, because it is settled from the manifest before the bundle
        // that would hold the file exists. Absolute, as AST::File::get_path() is
        std::filesystem::path entry_file;

        // is this compile for a test run, in which case **no file root becomes the program at all**.
        //
        // `main` is still emitted and is still the three-argument C one - the process globals have to be
        // captured, or `std::env::args()` inside a test reads a global nothing filled in - but its body is
        // the prologue and a `ret 0`. Whatever program the module happens to be does not run: a test asked
        // for is a test, not a test after the application it sits in.
        //
        // deliberately **not** a fourth answer inside file_is_entry, which owns *narrowing* - "which of
        // these files is the program" and "is there a program at all" are two questions, and folding the
        // second into the first is how a target-less module would have started answering false
        bool test_mode = false;

        // `main` dispatches on `ECO_INTERNAL_RUN_TEST`. only a linked runner wants that ladder;
        // the JIT path calls each test by address and must not emit it, or a leftover env var
        // would run a test inside the prologue
        bool emit_native_test_runner = false;

        // mangled names `echoc test` will call. empty on a `run`/`build`. a native runner's
        // `main` looks them up, and DCE cannot drop a test nothing in `main` appeared to reach
        std::vector<std::string> test_symbols;

        // the main compilation unit, or nullptr if the bundle has no main module yet
        CmpUnit *main_cmp_unit();

        // is this file the one whose root becomes `main`. **true for every file when no target named
        // one**, which is what keeps a target-less program the concatenation it has always been
        bool file_is_entry(const AST::File &file) const;

        std::string llvm_err_str();

        // a human-readable description of the current codegen location, e.g. "in function 'foo'"
        // or "at global scope", suffixed onto codegen error messages
        std::string function_context() const;

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
