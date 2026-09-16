#ifndef ENUMMAPCODEGEN_H
#define ENUMMAPCODEGEN_H

#pragma once

namespace AST
{
    class FunctionDeclNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // **the LUT lowering of a synthesized enum conversion.** `$key->glfw()`, `KeyCode::from(glfw:)`,
    // and closed `HttpStatus::from($raw)` share it. independent of O2: the tables are constant
    // globals this compiler writes, not a switch LLVM might rewrite
    //
    // returns true when the body was emitted, so StmtCodegen::gen_function_decl does not walk the
    // match / if-chain the parser planted for the semantic passes. false leaves that body in place:
    // a span too wide to tabulate, or values that did not fold (TypeChecker already refused those)
    bool try_gen_enum_lut(CodegenContext &ctx, AST::FunctionDeclNode &node);
};

#endif
