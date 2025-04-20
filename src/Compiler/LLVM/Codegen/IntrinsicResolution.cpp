#include "Compiler/LLVM/Codegen/IntrinsicResolution.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{
IntrinsicResolution resolve_intrinsic(const std::string &name, llvm::FunctionType *signature)
{
    IntrinsicResolution resolution;

    // the name half. lookupIntrinsicID knows every `llvm.*` LLVM was built with, so there is no
    // list here to fall behind the LLVM we link against
    resolution.id = llvm::Intrinsic::lookupIntrinsicID(name);

    if (resolution.id == llvm::Intrinsic::not_intrinsic) {
        resolution.failure = fmt::format("'{}' is not an intrinsic known to LLVM {}.", name, LLVM_VERSION_STRING);
        return resolution;
    }

    if (signature == nullptr) {
        resolution.failure = fmt::format("intrinsic '{}' cannot be resolved without a signature.", name);
        return resolution;
    }

    // the shape half. the IIT descriptors are the intrinsic's signature as its .td file declares
    // it, with the overloaded positions left as placeholders
    llvm::SmallVector<llvm::Intrinsic::IITDescriptor, 8> descriptors;
    llvm::Intrinsic::getIntrinsicInfoTableEntries(resolution.id, descriptors);

    // matchIntrinsicSignature *consumes* the range as it walks - return type first, then each
    // parameter - so it takes a mutable ArrayRef and what is left over afterwards is meaningful
    llvm::ArrayRef<llvm::Intrinsic::IITDescriptor> remaining(descriptors);

    const auto match = llvm::Intrinsic::matchIntrinsicSignature(signature, remaining, resolution.overload_types);

    if (match == llvm::Intrinsic::MatchIntrinsicTypes_NoMatchRet) {
        resolution.failure = fmt::format("return type does not match intrinsic '{}'.", name);
        return resolution;
    }

    if (match == llvm::Intrinsic::MatchIntrinsicTypes_NoMatchArg) {
        // this arm is also how *too many* arguments are caught: matchIntrinsicType refuses as soon
        // as the descriptors run out, so an extra parameter is a mismatched argument rather than a
        // silently ignored one
        resolution.failure = fmt::format("arguments do not match intrinsic '{}'.", name);
        return resolution;
    }

    // and too *few* arguments leave descriptors unconsumed, which only this second call notices.
    // the pair is the sequence the IR verifier uses; matching the signature alone would accept a
    // one-argument declaration of a two-argument intrinsic
    if (llvm::Intrinsic::matchIntrinsicVarArg(signature->isVarArg(), remaining)) {
        resolution.failure = fmt::format("wrong number of arguments for intrinsic '{}'.", name);
        return resolution;
    }

    return resolution;
}

llvm::Function *declare_intrinsic(llvm::Module *module, const std::string &name, llvm::FunctionType *signature, std::string &failure)
{
    const IntrinsicResolution resolution = resolve_intrinsic(name, signature);

    if (!resolution.ok()) {
        failure = resolution.failure;
        return nullptr;
    }

    failure.clear();

    return llvm::Intrinsic::getOrInsertDeclaration(module, resolution.id, resolution.overload_types);
}
};
