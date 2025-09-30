#ifndef BACKEND_H
#define BACKEND_H

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm
{
    class TargetMachine;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    struct CmpUnit;

    // the output stage of the compiler: runs the optimization pipeline, prints the module IR,
    // JIT-executes the main module, and emits a native executable
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

        // emits the object file and links it into `executable_name`, false on any of its three
        // failure paths. it reports by return value rather than only by printing, because a caller
        // that cannot tell exits 0 having produced no binary - which is a build that looks
        // successful to a shell, a Makefile and the e2e suite alike
        //
        // the whole-program spelling: one unit, one object, one link. Kept for the paths that merged
        // everything into main first - `-O` and `--print-ir` - where per-module objects do not exist
        bool make_exec(std::string executable_name);

        // one unit to one object file. **Sound only because an ODR-shared definition is emitted into
        // every unit that references it**: without that a unit's object would be missing the bodies its
        // callers expect somebody else to have provided
        bool emit_object(CmpUnit &cmp_unit, const std::filesystem::path &object_path);

        // links objects into an executable. Prefers the system linker and falls back to the `clang`
        // driver, which is a ~33ms difference on every build: almost all of `clang -o exe exe.o` is
        // driver startup, and this stage is otherwise a constant floor no amount of caching removes.
        // The fallback is what keeps a platform whose flags we cannot spell buildable rather than broken
        bool link_executable(
            const std::string &executable_name, const std::vector<std::filesystem::path> &objects);

    private:
        CodegenContext &_ctx;

        // the host target, resolved once by init_target. object emission needs one too and it
        // must describe the same target as the layout codegen ran against, so it is the same
        // instance rather than a second lookup
        std::unique_ptr<llvm::TargetMachine> _target_machine;
    };
};

#endif
