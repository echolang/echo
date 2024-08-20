#ifndef BACKEND_H
#define BACKEND_H

#pragma once

#include <string>

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the output stage of the compiler: runs the optimization pipeline, prints the module IR,
    // JIT-executes the main module, and emits a native executable (via clang).
    class Backend
    {
    public:
        Backend(CodegenContext &ctx) : _ctx(ctx) {};

        void optimize();
        void print_ir(bool to_file);
        void run_code();
        void make_exec(std::string executable_name);

    private:
        CodegenContext &_ctx;
    };
}

#endif
