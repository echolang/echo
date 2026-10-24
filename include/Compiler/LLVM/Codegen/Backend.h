#ifndef BACKEND_H
#define BACKEND_H

#pragma once

#include "Compiler/LinkRequirement.h"
#include "Compiler/TargetSubtarget.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llvm
{
    class ExecutionEngine;
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

        // resolves this invocation's target and publishes its data layout on the context, so every
        // llvm::Module can be created with a layout already attached. must run before
        // create_cmp_units
        //
        // the layout cannot wait until emission, after all IR has been built, and cannot be
        // skipped on the JIT path - anything asking "how big is this type" during codegen would
        // get LLVM's default layout (which aligns i64 to 4, unlike any real 64-bit target) and
        // the optimizer would run over layout-less modules
        void init_target();

        // which CPU everything downstream compiles for, resolved off the options rather than held: the
        // two callers are init_target, which builds the one TargetMachine, and the JIT, which
        // builds a second one it will not take from us. See Compiler::resolve_subtarget for why this
        // is asked twice rather than stored once
        Compiler::Subtarget subtarget() const;

        void optimize();

        // **everything that happens to a unit's module between codegen and its object**, which is two
        // things and only one of them is optional: the definitions this unit does not reference are
        // dropped always, and the baseline pipeline runs unless `--optimize none` was asked for. See the
        // implementation for why the discard cannot be left to the pipeline, and for why the pipeline is
        // per unit and what that buys the object cache. Idempotent - `CmpUnit::optimized` is the flag
        void prepare_unit_for_emission(Compiler::LLVM::CmpUnit &cmp_unit);

        void print_ir(bool to_file);

        // the per-unit dump, for the build path that has no merge - see the implementation
        void print_unit_ir();

        // prune, build the engine, finalize objects. idempotent; false with a message already printed.
        // the module is gone afterwards. native libraries must already be open: MCJIT hangs on a missing one
        bool prepare_execution();

        // calls the entry point.
        //
        // `arguments` is the program's whole `argv`, its own name at index 0 included, and `environment`
        // is a null-terminated block in `envp` shape. Both are the *program's*, not echoc's: under `run`
        // there is no process boundary to separate them, so the driver has to say which is which - it
        // splits its own command line on `--` and forwards the tail. Handing the program echoc's argv
        // instead would have `env::arg(1)` answer with a compiler flag
        //
        // returns what the entry point returned, so `echoc run` exits the way the program did. A
        // module-scope `die` or `env::exit` never reaches this - both call libc's `exit` from inside the
        // JIT'd code, which takes echoc down with them, and that is already the right exit status
        int run_main(const std::vector<std::string> &arguments, const char *const *environment);

        // the address of a finalized definition, or 0 when the engine holds no such symbol.
        //
        // **0 is a real answer and a caller must refuse on it**: the prune deletes anything its root set
        // does not reach, so a symbol asked for without being named a root is not there - which is a
        // mistake in the root set, and calling through a null pointer is how it would present
        uint64_t function_address(const std::string &mangled) const;

        // the symbols the JIT's prune keeps besides the entry point.
        //
        // empty for a program, whose one root is `main` by definition. A test run's roots are the tests it
        // was asked to run, and stating them here rather than keeping every definition is what makes
        // `--filter` narrow the machine code as well as the run
        void set_jit_roots(std::vector<std::string> roots) {
            _jit_roots = std::move(roots);
        }

        // what the prune dropped and what survived it: the `[prune]` section `--explain-prune` asks for,
        // empty until prepare_execution has pruned - which is what makes it print nothing on a `build`.
        //
        // a string rather than a print, the shape PhaseTimings::report() already uses: which diagnostics
        // a compile prints is the driver's question, and the driver is the only place that sees the flag
        const std::string &prune_report() const { return _prune_report; }

        // **one unit to one object file, and one link, and that is the whole of emission.** a
        // third entry point sitting beside these two would be exactly the pair of them over the
        // one unit a merge leaves behind, so the driver would branch between two spellings of one
        // path and the one exercised least would be the one a change would miss.
        //
        // both report by return value rather than only by printing, because a caller that cannot tell
        // exits 0 having produced no binary - a build that looks successful to a shell, a Makefile and
        // the e2e suite alike.
        //
        // **Sound only because an ODR-shared definition is emitted into every unit that references
        // it**: without that a unit's object would be missing the bodies its callers expect somebody
        // else to have provided. Its converse is prepare_unit_for_emission's discard - a unit that does
        // *not* reference one has no business shipping it
        bool emit_object(CmpUnit &cmp_unit, const std::filesystem::path &object_path);

        // links objects into a program (command or library). Prefers the system linker and falls back to the `clang`
        // driver, which is a ~33ms difference on every build: almost all of `clang -o exe exe.o` is
        // driver startup, and this stage is otherwise a constant floor no amount of caching removes.
        // The fallback is what keeps a platform whose flags we cannot spell buildable rather than broken
        //
        // `link` is what the build's manifests and command line asked for, already merged and ordered by
        // the driver. **Both spellings render it through Compiler::partition_link_requirements** - the
        // fallback is the path exercised least, so a second hand-written rendering there is one that
        // drifts without anybody noticing until a platform needs it
        //
        // `reuse_record`: keep the binary if the link line did not move. not under `-g`, and only when every
        // library is a dynamic `runtime: process` one. keyed on the command that would run (host `ld` /
        // `lld-link`, or the clang driver Linux always takes) plus object paths
        // (`Compiler::link_inputs_digest`); filenames already carry the content hex
        bool link_program(
            const std::string &program_name,
            const std::vector<std::filesystem::path> &objects,
            const std::vector<Compiler::LinkRequirement> &link,
            const std::filesystem::path *reuse_record
        );

    private:
        // **Mach-O only, and only under `-g`.** `ld` leaves the DWARF in the objects and writes only a
        // debug map into the binary, so a debug session depends on those objects still being where they
        // were linked from. dsymutil is what folds them into a self-contained `<exe>.dSYM`. Best effort:
        // a toolchain without it still produces a binary that runs. On ELF this is a no-op, because the
        // linker puts the DWARF in the executable itself
        void gen_debug_symbols(const std::string &executable_name);

        // JIT only: drop everything the entry and set_jit_roots cannot reach. a `build` must not:
        // a library object may not depend on which application consumes it. `-p` dumps the module
        // this has not run on; `--explain prune` is the other answer
        void prune_to_entry();

        CodegenContext &_ctx;

        // this invocation's target, resolved once by init_target. object emission needs one too
        // and it must describe the same target as the layout codegen ran against, so it is the
        // same instance rather than a second lookup
        std::unique_ptr<llvm::TargetMachine> _target_machine;

        // built by prune_to_entry, read by prune_report. accumulated unconditionally: it costs one walk
        // of the function list, and a diagnostic that only computes itself when asked for is a second
        // code path through the thing it is meant to describe
        std::string _prune_report;

        // the roots the prune keeps beside the entry point - see set_jit_roots
        std::vector<std::string> _jit_roots;

        // the live JIT, from prepare_execution until this Backend is destroyed. **it owns the main module**
        // from the moment EngineBuilder takes it, which is why nothing may print or emit IR afterwards
        llvm::ExecutionEngine *_engine = nullptr;
    };
};

#endif
