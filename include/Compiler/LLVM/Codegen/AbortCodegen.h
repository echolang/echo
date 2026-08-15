#ifndef ABORTCODEGEN_H
#define ABORTCODEGEN_H

#pragma once

#include "Token.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>

#include <string>

namespace AST
{
    class FunctionCallExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the one owner of how a program stops
    //
    // three sites stop a program and they used to share nothing: `die`, a failed `assert`, and the
    // null check the `ptr<T>` -> `T&` narrowing emits. the last of those was an `llvm.trap` with no
    // message, gated on how *echoc itself* had been compiled. collecting them here is what makes
    // one runtime, one message shape and one release-mode gate possible - and it is why this is a
    // subsystem rather than two more private methods on ExprCodegen
    //
    // a compile-time message and a runtime `die` take the same `__eco_abort`: headline, detail
    // and location as separate pieces. the crash path never allocates a combined string
    class AbortCodegen
    {
    public:
        AbortCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // stop, unconditionally. emits the piece globals, the call, and `unreachable` - which is
        // the whole of what a non-returning call owes the rest of codegen: StmtCodegen::gen_scope
        // stops emitting a scope whose block is terminated, gen_function_decl only synthesizes a
        // trailing return when there is no terminator, and the if/while arms already guard every
        // branch on the same question. nothing else needed a change for `die` to be legal anywhere
        //
        // the message is taken in *parts* rather than finished, because that is what makes the
        // "one message shape" claim above true rather than aspirational: the null check used to
        // hand-format its own and had already drifted to `in {}` where the other two said `at {}`,
        // on the day both were written. `detail` may be empty, and then the headline stands alone
        void gen_abort(
            const std::string &headline,
            const std::string &detail,
            const TokenReference &at);

        // the same stop, when the detail is a `string` the program computed. the location is
        // still the call site. same thunk as gen_abort - only the detail operand differs
        void gen_abort_dynamic(
            const std::string &headline,
            llvm::Value *detail_ptr,
            llvm::Value *detail_len,
            const TokenReference &at);

        // stop with a chosen exit code, printing nothing. what `std::env::exit` lowers to
        //
        // here rather than in ProcessCodegen because *how a program stops* is this subsystem's question,
        // and `exit` is the symbol it already owns - a second declaration of it elsewhere would be two
        // spellings of one name with only one of them carrying NoReturn
        //
        // no flush, unlike the abort thunk: libc's `exit` flushes every open stream on its way out, and
        // the only reason `__eco_abort` does it by hand is that its message goes out through `write(2)`
        // and would otherwise overtake the buffered `echo`s ahead of it
        void gen_exit(llvm::Value *code);

        // stop when `condition` is true, and leave the builder on the path where it was not. this is
        // the shape the null check has always had; `assert` is the same shape with the condition
        // negated
        void gen_abort_if(
            llvm::Value *condition,
            const std::string &headline,
            const std::string &detail,
            const TokenReference &at);

        // the crash-hook store. `swap` is set_hook: write `fn`, return what was there (null if
        // the default report was in place). `take` writes null. both are the only writers of
        // `__eco_crash_hook`
        llvm::Value *swap_hook(llvm::Value *fn);
        llvm::Value *take_hook();

        // the default report, no exit. what `crash::default_hook` lowers to, so a wrapper can
        // print then do more. the thunk itself prints through write_pieces when no hook is set
        void gen_default_hook(llvm::Value *info);

        // the text a `die`/`assert` call site folds in, or "" when the call carries none. the
        // literal is read rather than lowered - the whole message is a compile-time constant, so
        // emitting the argument would produce a second, unused global
        std::string detail_of(const AST::FunctionCallExprNode &node) const;

    private:
        CodegenContext &_ctx;

        // the one abort implementation per compilation unit, created on first use
        //
        // always piece-wise:
        //   void __eco_abort(ptr headline, i64, ptr msg, i64, ptr file, i64, i32 line, ptr line.text, i64)
        //
        // `#[core: crash_info]` decides whether the *body* loads `__eco_crash_hook`, not the
        // signature. `--no-stdlib` is not a second ABI - unwrap_abort never needed a `string`
        // type, and piece-wise writes do not either. every unit emits the same type and
        // `linkonce_odr` stays sound
        llvm::Function *get_or_create_abort_thunk();

        // `exit` is the one libc symbol here that needs more than CodegenContext::libc_callee gives:
        // NoReturn, so the `unreachable` after the call is a fact rather than a promise. `fflush`
        // and `write` are the two helpers below
        llvm::FunctionCallee get_exit();

        void flush_stdout();
        void write_stderr(llvm::Value *ptr, llvm::Value *len);

        bool hooks_enabled() const;
        llvm::GlobalVariable *hook_global();

        void emit_thunk(llvm::Function *thunk);

        void call_thunk(
            const std::string &headline,
            llvm::Value *detail_ptr,
            llvm::Value *detail_len,
            const TokenReference &at);

        void store_view(
            llvm::Value *info,
            llvm::StructType *info_ty,
            llvm::StructType *view_ty,
            size_t field,
            llvm::Value *bytes,
            llvm::Value *len);

        void emit_default_print_body(llvm::Value *info);

        // unnamed structs whose field order matches the bound crash::info / string::view.
        // a named TypeDecl struct is interned per unit, so a linkonce_odr body that GEPs one
        // would differ across modules and fail the ODR check
        void crash_llvm_types(llvm::StructType *&info_ty, llvm::StructType *&view_ty);

        void write_pieces(
            llvm::Value *headline, llvm::Value *headline_len,
            llvm::Value *message, llvm::Value *message_len,
            llvm::Value *file, llvm::Value *file_len,
            llvm::Value *line, llvm::Value *line_len);
    };
};

#endif
