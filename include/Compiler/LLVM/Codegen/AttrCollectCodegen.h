#ifndef ATTRCOLLECTCODEGEN_H
#define ATTRCOLLECTCODEGEN_H

#pragma once

namespace AST
{
    class FunctionCallExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // **the table `attr::of` returns.** a constant slice global in the entry unit, named by
    // attr_table_symbol. other units declare it. AbiCodegen::emit_tables is the same moment
    void emit_attr_tables(CodegenContext &ctx);
    void gen_attr_of_builtin(CodegenContext &ctx, AST::FunctionCallExprNode &node);
};

#endif
