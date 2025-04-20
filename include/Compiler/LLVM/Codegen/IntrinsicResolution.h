#ifndef INTRINSICRESOLUTION_H
#define INTRINSICRESOLUTION_H

#pragma once

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Intrinsics.h>

#include <string>

namespace llvm
{
    class FunctionType;
    class Module;
    class Type;
};

namespace Compiler::LLVM
{
    // the one owner of "which LLVM intrinsic does this name denote, and does a given signature fit
    // it"
    //
    // it used to be a hand-written string -> ID table that answered only the first half, and the
    // second half was *guessed*: the whole argument list was handed to getOrInsertDeclaration as
    // the overload-type vector. that conflates two different counts. `llvm.pow` is
    // `T @llvm.pow.T(T, T)` - two arguments, one overload type - so a two-argument declaration
    // mangled a name LLVM never defined, and the same held for copysign, minnum, fma, ctlz, powi
    // and ldexp. an intrinsic with more than one argument was effectively undeclarable, which is
    // the real reason the math stdlib had three functions in it
    //
    // the return type was not consulted at all, so nothing caught a declaration whose Echo result
    // type disagreed with the intrinsic's, and an unknown name became not_intrinsic and went
    // straight into getOrInsertDeclaration to assert somewhere inside LLVM
    //
    // asking LLVM's own IIT tables answers all three at once, because that is what the IR verifier
    // and the .ll parser ask
    //
    // this lives in the Compiler::LLVM layer rather than in AST on purpose. include/AST and src/AST
    // contain no LLVM include at all, and an authoritative answer needs LLVM's intrinsic tables -
    // pulling LLVM across that boundary to move one diagnostic earlier is not a trade worth making
    struct IntrinsicResolution
    {
        llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;

        // *derived* from the signature by matchIntrinsicSignature, never the argument list. only
        // the overloaded positions appear here, which is exactly what getOrInsertDeclaration wants
        llvm::SmallVector<llvm::Type *, 2> overload_types;

        // empty on success. a reason rather than a bool, so the caller can say *why* instead of
        // letting LLVM assert
        std::string failure;

        bool ok() const {
            return failure.empty();
        }
    };

    // `signature` is the whole Echo signature lowered to LLVM - the return type and every
    // parameter - because the return type participates in the match
    IntrinsicResolution resolve_intrinsic(const std::string &name, llvm::FunctionType *signature);

    // resolve and declare in one step, for the callers that have no use for a partial answer.
    // returns nullptr and fills `failure` when resolution fails, so a caller that wants to throw
    // still can
    llvm::Function *declare_intrinsic(llvm::Module *module, const std::string &name, llvm::FunctionType *signature, std::string &failure);
};

#endif
