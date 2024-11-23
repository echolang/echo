#ifndef BACKEND_H
#define BACKEND_H

#pragma once

#include <memory>
#include <string>

namespace llvm
{
    class TargetMachine;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the output stage of the compiler: runs the optimization pipeline, prints the module IR,
    // JIT-executes the main module, and emits a native executable (via clang)
    class Backend
    {
    public:
        Backend(CodegenContext &ctx);
        ~Backend();

        // resolves the host target and publishes its data layout on the context, so every
        // llvm::Module can be created with a layout already attached. must run before
        // create_cmp_units
        //
        // the layout used to be set only in make_exec, i.e. after all IR had been built and never
        // at all on the JIT path - so anything asking "how big is this type" during codegen got
        // LLVM's default layout (which aligns i64 to 4, unlike any real 64-bit target) and the
        // optimizer ran over layout-less modules
        void init_target();

        void optimize();
        void print_ir(bool to_file);
        void run_code();
        void make_exec(std::string executable_name);

    private:
        CodegenContext &_ctx;

        // the host target, resolved once by init_target. object emission needs one too and it
        // must describe the same target as the layout codegen ran against, so it is the same
        // instance rather than a second lookup
        std::unique_ptr<llvm::TargetMachine> _target_machine;
    };
};

#endif
