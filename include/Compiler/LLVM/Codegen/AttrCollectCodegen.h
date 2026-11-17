#ifndef ATTRCOLLECTCODEGEN_H
#define ATTRCOLLECTCODEGEN_H

#pragma once

#include <string>

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

    // **the tables the module `root` would have collected in a build of its own**, made the ones
    // `attr::of` returns from here on. a test runner over several roots is one program holding
    // every root's declarations, and each test still has to find the program its own root is: a
    // registry filled from "everything tagged" is otherwise filled from twenty-seven strangers too.
    // emitted at the builder's position, in the entry unit, before the test is called. nothing when
    // the bundle is one program, which is every build but that one
    void emit_attr_view_selection(CodegenContext &ctx, const std::string &root);
    void gen_attr_of_builtin(CodegenContext &ctx, AST::FunctionCallExprNode &node);
};

#endif
