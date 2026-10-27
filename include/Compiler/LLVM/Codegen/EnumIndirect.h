#ifndef ENUMINDIRECT_H
#define ENUMINDIRECT_H

#pragma once

#include "Compiler/LLVM/Codegen/CodegenValue.h"

namespace llvm
{
    class Value;
};

namespace Compiler::LLVM
{
    struct CodegenContext;
    struct Structure;

    // **an indirect enum case's heap cell.** no class header: one allocation of the payload
    // fields, a pointer in the overlay. alloc is planted after the tag write in the case
    // constructor and at the start of each heap-case copy branch; free is the planted
    // builtin at the end of that case's deinit
    namespace EnumIndirect
    {
        // allocate a cell for the live heap case (tag already written) and store the overlay
        void alloc_cell(CodegenContext &ctx, const Structure &structure, const LValue &enum_base);

        // the cell address for this heap-case field. debug-aborts on a null overlay
        llvm::Value *load_cell(
            CodegenContext &ctx,
            const Structure &structure,
            const LValue &enum_base,
            size_t property_index
        );

        // load the overlay pointer and free it. null is a no-op, matching MemoryCodegen::gen_free
        void free_cell(CodegenContext &ctx, const Structure &structure, const LValue &enum_base);
    };
};

#endif
