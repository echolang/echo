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
    // which LLVM intrinsic this name denotes, and whether the signature fits. asked of LLVM's IIT
    // tables: argument count is not overload-type count (`llvm.pow` is two args, one T)
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
