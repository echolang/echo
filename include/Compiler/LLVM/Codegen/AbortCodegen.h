#ifndef ABORTCODEGEN_H
#define ABORTCODEGEN_H

#pragma once

#include <llvm/IR/Function.h>
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
    // the message is always composed at compile time, because everything in it is known then: the
    // literal the user wrote and the call site's token. so the runtime takes a pointer and a length
    // and knows nothing about formatting
    class AbortCodegen
    {
    public:
        AbortCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // stop, unconditionally. emits the message global, the call, and `unreachable` - which is
        // the whole of what a non-returning call owes the rest of codegen: StmtCodegen::gen_scope
        // stops emitting a scope whose block is terminated, gen_function_decl only synthesizes a
        // trailing return when there is no terminator, and the if/while arms already guard every
        // branch on the same question. nothing else needed a change for `die` to be legal anywhere
        //
        // the message is taken in *parts* rather than finished, because that is what makes the
        // "one message shape" claim above true rather than aspirational: the null check used to
        // hand-format its own and had already drifted to `in {}` where the other two said `at {}`,
        // on the day both were written. `detail` may be empty, and then the headline stands alone
        void gen_abort(const std::string &headline, const std::string &detail, const std::string &location);

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
        void gen_abort_if(llvm::Value *condition,
            const std::string &headline, const std::string &detail, const std::string &location);

        // "<file>:<line>" for a call site, the suffix every message carries
        //
        // the file is the one being emitted, which for a call inside an instantiated generic need
        // not be the file it was written in - todo/C6 is the fix, and it is a real hole: the
        // location it prints in that case exists, and is not where the call is
        std::string location_of(const AST::FunctionCallExprNode &node) const;

        // the text a `die`/`assert` call site folds in, or "" when the call carries none. the
        // literal is read rather than lowered - the whole message is a compile-time constant, so
        // emitting the argument would produce a second, unused global
        std::string detail_of(const AST::FunctionCallExprNode &node) const;

    private:
        CodegenContext &_ctx;

        // the one abort implementation per compilation unit, created on first use
        //
        //   void __eco_abort(ptr msg, i64 len)
        //
        // flush, write to stderr, exit(1). `linkonce_odr` for the reason the release thunks are:
        // every unit that can stop emits its own definition and the linker folds them
        llvm::Function *get_or_create_abort_thunk();

        // `exit` is the one libc symbol here that needs more than CodegenContext::libc_callee gives:
        // NoReturn, so the `unreachable` after the call is a fact rather than a promise. `fflush`
        // and `write` are plain calls made inline in the thunk
        llvm::FunctionCallee get_exit();
    };
};

#endif
