#ifndef ODRCOMPARISON_H
#define ODRCOMPARISON_H

#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

#include <optional>
#include <string>

namespace Compiler::LLVM
{
    // where two definitions of one symbol stopped agreeing. `what` names the thing that diverged and
    // the position it diverged at - "block 3, instruction 5: operand" - because the two rendered
    // bodies a human is handed alongside it are long, and a diff of them is what somebody
    // would otherwise have to do by eye
    struct OdrDifference
    {
        std::string what;

        // null when the difference is in the signature rather than in the body
        const llvm::Instruction *left = nullptr;
        const llvm::Instruction *right = nullptr;
    };

    // are these two llvm::Functions the same definition, in two different modules.
    // nothing a module owns is compared by pointer: a value is a position, a global is a name
    // (or its initializer, when the name is module-local). the LLVMContext is shared, so a type
    // and an AttributeList compare by identity
    std::optional<OdrDifference> first_odr_difference(const llvm::Function &left, const llvm::Function &right);
};

#endif
