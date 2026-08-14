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
    // bodies a human is handed alongside it are long, and a diff of them is what this used to make
    // somebody do by eye
    struct OdrDifference
    {
        std::string what;

        // null when the difference is in the signature rather than in the body
        const llvm::Instruction *left = nullptr;
        const llvm::Instruction *right = nullptr;
    };

    // **are these two llvm::Functions the same definition** - asked of two functions in two different
    // modules, which is the whole of why this is written here rather than taken from LLVM.
    //
    // `llvm::FunctionComparator` is the same question inside one module and cannot be borrowed for
    // this one: it compares a referenced global through `GlobalNumberState`, which hands every
    // distinct `GlobalValue *` its own number on first sight - so two modules' `@__eco_abort` are two
    // numbers and every body that references anything at all compares unequal. Nothing in it is
    // virtual, so a subclass cannot change that. `llvm::StructuralHash` is a different shape of no:
    // it hashes opcodes, block structure, type *ids*, integer and float constants and the names of
    // called functions, and is blind to a `GlobalVariable` operand, a struct's layout, an
    // `AttributeList` and every kind of metadata - which is precisely the set this check exists to
    // watch.
    //
    // so the rule throughout is: **nothing a module owns may be compared by pointer.** A value is a
    // position, a global is a name or - where the name is module-local and therefore says nothing -
    // its content. What *is* shared is the `LLVMContext`, so a type, an `AttributeList` and a uniqued
    // MDNode do compare by identity, and only the first of those needs a fallback beneath it.
    //
    // returns nothing when the two are the same definition. Nothing is rendered on that path
    std::optional<OdrDifference> first_odr_difference(const llvm::Function &left, const llvm::Function &right);
};

#endif
