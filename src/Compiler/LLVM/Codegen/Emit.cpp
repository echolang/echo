#include "Compiler/LLVM/Codegen/Emit.h"

#include "Compiler/JobCount.h"
#include "Compiler/LLVM/Codegen/Backend.h"
#include "Compiler/LLVM/Codegen/Bitcode.h"
#include "Compiler/LLVM/Codegen/IsolatedEmit.h"
#include "Compiler/LLVM/Codegen/LibraryVisibility.h"
#include "Compiler/LLVM/Codegen/Partition.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/ModuleCache.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/ProgressReporter.h"
#include "Compiler/TargetSubtarget.h"

#include "AST/ASTModule.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <unordered_set>

namespace Compiler::LLVM
{

// ISel workers for a library unit. packing files around `__shared` does not shrink
// the shared object if that is what actually takes the time; 8 is a cap on machines
static constexpr size_t k_library_partitions = 8;

static bool host_bitcode_round_trip_works()
{
    // a Windows-hosted LLVM fails the same-process bitcode round-trip,
    // whatever the row emits
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

static IsolatedEmitRequest make_isolated_request(
    std::string bitcode,
    std::filesystem::path object_path,
    const CodegenContext &ctx,
    const Compiler::Subtarget &sub,
    bool already_optimized,
    bool timings
)
{
    IsolatedEmitRequest request;
    request.bitcode = std::move(bitcode);
    request.object_path = std::move(object_path);
    request.triple = ctx.target_triple;
    request.cpu = sub.cpu;
    request.features = sub.features;
    request.codegen_opt = ctx.options.codegen_opt;
    request.pic = ctx.options.codegen.uses_pic();
    request.targeting_windows = ctx.targeting_windows();
    request.already_optimized = already_optimized;
    request.timings = timings;
    return request;
}

// one object a split unit is made of
struct PartitionPiece
{
    std::filesystem::path object_path;
    // empty when the object is already there
    std::string bitcode;
};

// the pieces of `parts`, one per distinct hash, each materialized unless its object is a hit
static std::vector<PartitionPiece> partition_pieces(
    CmpUnit &unit,
    llvm::Module &module,
    const std::vector<UnitPartition> &parts,
    const std::filesystem::path &dest,
    const CodegenContext &ctx
)
{
    std::vector<PartitionPiece> pieces;
    std::unordered_set<std::string> seen;

    for (const UnitPartition &part : parts) {
        if (!seen.insert(part.hex).second) {
            continue;
        }

        PartitionPiece piece;
        piece.object_path = dest.parent_path() / ("p." + part.hex + ".o");

        if (!Compiler::cached_object_is_hit(piece.object_path)) {
            piece.bitcode = materialize_partition(unit, module, part, ctx);
        }

        pieces.push_back(std::move(piece));
    }

    return pieces;
}

// a library unit, optimized whole and then split, in a worker's own LLVMContext: splitting first
// loses inlining across its files, and ISel is the half that fans out
struct LibrarySplitRequest
{
    CmpUnit *unit = nullptr;
    // the whole unit, to its own object
    IsolatedEmitRequest whole;
    // index into emit_unit_objects' outputs
    size_t output = 0;
};

struct LibrarySplitResult
{
    IsolatedEmitResult prepared;
    std::vector<PartitionPiece> pieces;
};

// reads `request.unit` and `ctx` and writes neither: Partition's function_homes asks the unit's
// function table by name and the AST where a declaration was written
static LibrarySplitResult split_library_unit(
    const LibrarySplitRequest &request,
    const CodegenContext &ctx,
    const PartitionEnv &env
)
{
    LibrarySplitResult result;
    llvm::LLVMContext context;
    IsolatedModule loaded;

    if (!load_isolated_module(request.whole, context, loaded, result.prepared)) {
        return result;
    }

    // nothing optimizes a piece after this, so no piece carries a copy for an inliner
    const std::vector<UnitPartition> parts =
        partition_unit(*request.unit, *loaded.module, ctx, env, k_library_partitions, false);

    if (parts.size() < 2) {
        result.pieces.push_back({ request.whole.object_path, bitcode_of(*loaded.module) });
        return result;
    }

    result.pieces = partition_pieces(*request.unit, *loaded.module, parts, request.whole.object_path, ctx);
    return result;
}

static bool run_isolated_emits(std::vector<IsolatedEmitRequest> &requests, unsigned jobs)
{
    if (requests.empty()) {
        return true;
    }

    Compiler::PhaseTimings &clock = Compiler::PhaseTimings::instance();
    std::vector<IsolatedEmitResult> results(requests.size());

    // isolated workers parse their own bitcode and never write this stream, so the live row stays
    // drawn through ISel. a failed unit suspends below before the dump
    Compiler::run_jobs(jobs, requests.size(), [&](size_t i) {
        results[i] = emit_isolated_unit(requests[i]);
    });

    for (size_t i = 0; i < results.size(); i++) {
        if (!results[i].ok) {
            Compiler::ProgressReporter::instance().suspend();
            llvm::errs() << results[i].error << "\n";
            return false;
        }

        clock.record("optimize", results[i].optimize_ms);
        clock.record("machine code", results[i].isel_ms);

        clock.record_slowest("optimize", results[i].slowest_optimize);
        clock.record_slowest("machine code", results[i].slowest_isel);
    }

    return true;
}

bool emit_unit_objects(
    CodegenContext &ctx,
    Backend &backend,
    const std::function<std::filesystem::path(const std::string &)> &object_for,
    std::vector<std::filesystem::path> &out_objects
)
{
    Compiler::ScopedPhase phase("emit objects");

    // before partition and before an isolated snapshot, so every object of a
    // native library is hidden-except-export even when prepare_unit runs in a
    // worker that never sees this Backend
    if (ctx.options.codegen.is_native_library()) {
        hide_non_exported_symbols(ctx);
    }

    const unsigned jobs = Compiler::job_count();
    const Compiler::Subtarget sub = backend.subtarget();
    const bool timings = Compiler::PhaseTimings::instance().enabled();

    PartitionEnv env;
    env.triple = ctx.target_triple;
    env.cpu = sub.cpu;
    env.features = sub.features;
    env.no_optimize = ctx.options.no_optimize();
    env.targeting_windows = ctx.targeting_windows();
    env.cross = ctx.options.codegen.is_cross();
    env.exec_model = Compiler::exec_model_environment(ctx.options.codegen);

    struct Output
    {
        std::filesystem::path dest;
        std::vector<std::filesystem::path> pieces;
        CmpUnit *whole = nullptr;
    };

    std::vector<Output> outputs;
    std::vector<IsolatedEmitRequest> isolated;

    std::vector<LibrarySplitRequest> libraries;

    // a split unit's pieces join its output, and the ones not already there are queued for ISel
    auto queue_pieces = [&](Output &output, std::vector<PartitionPiece> &pieces, bool already_optimized) {
        for (PartitionPiece &piece : pieces) {
            output.pieces.push_back(piece.object_path);

            if (!piece.bitcode.empty()) {
                isolated.push_back(make_isolated_request(
                    std::move(piece.bitcode), piece.object_path, ctx, sub, already_optimized, timings));
            }
        }
    };

    for (auto &cmp_unit : ctx.cmp_units) {
        if (!cmp_unit->llvm_module) {
            continue;
        }

        Output output;
        output.dest = object_for(cmp_unit->ast_module->name);

        const bool can_split = can_join_relocatable(env)
            && cmp_unit->ast_module != nullptr
            && !cmp_unit->optimized
            && !cmp_unit->merged
            && !ctx.options.debug_info;

        // a library is optimized whole, then only ISel fans out: split earlier and you lose inlining.
        // both happen in split_library_unit, beside every other library's
        if (can_split && cmp_unit->ast_module->name != ctx.entry_module_name) {
            LibrarySplitRequest request;
            request.unit = cmp_unit.get();
            request.whole = make_isolated_request(
                bitcode_of(*cmp_unit->llvm_module), output.dest, ctx, sub, false, timings);
            request.output = outputs.size();

            cmp_unit->llvm_module.reset();
            cmp_unit->optimized = true;

            libraries.push_back(std::move(request));
            outputs.push_back(std::move(output));
            continue;
        }

        std::vector<UnitPartition> parts;
        if (can_split) {
            // the entry unit's pieces are optimized after the split, unless nothing is. see partition_unit
            parts = partition_unit(*cmp_unit, *cmp_unit->llvm_module, ctx, env, 0, !env.no_optimize);
        }

        if (parts.size() >= 2) {
            std::vector<PartitionPiece> pieces =
                partition_pieces(*cmp_unit, *cmp_unit->llvm_module, parts, output.dest, ctx);
            queue_pieces(output, pieces, false);

            cmp_unit->llvm_module.reset();
            cmp_unit->optimized = true;
        }
        else {
            output.pieces.push_back(output.dest);
            output.whole = cmp_unit.get();
        }

        outputs.push_back(std::move(output));
    }

    if (!libraries.empty()) {
        Compiler::PhaseTimings &clock = Compiler::PhaseTimings::instance();
        std::vector<LibrarySplitResult> splits(libraries.size());

        Compiler::run_jobs(jobs, libraries.size(), [&](size_t i) {
            splits[i] = split_library_unit(libraries[i], ctx, env);
            // the unit's IR is in its pieces now
            libraries[i].whole.bitcode = std::string();
        });

        for (size_t i = 0; i < splits.size(); i++) {
            if (!splits[i].prepared.ok) {
                Compiler::ProgressReporter::instance().suspend();
                llvm::errs() << splits[i].prepared.error << "\n";
                return false;
            }

            clock.record("optimize", splits[i].prepared.optimize_ms);
            clock.record_slowest("optimize", splits[i].prepared.slowest_optimize);

            queue_pieces(outputs[libraries[i].output], splits[i].pieces, true);
        }
    }

    std::vector<Output *> live;
    for (Output &output : outputs) {
        if (output.whole != nullptr) {
            live.push_back(&output);
        }
    }

    // a single live module, or `ECO_JOBS=1`, stays in this LLVMContext. the bitcode
    // round-trip is the cost; run_jobs would keep one item on this thread either way.
    // the row says whether the target snapshots; the host check is this process's LLVM
    const bool in_memory =
        !host_bitcode_round_trip_works()
        || !ctx.options.codegen.snapshots_bitcode()
        || live.size() <= 1
        || jobs <= 1;

    if (in_memory) {
        for (Output *output : live) {
            {
                Compiler::ScopedPhase optimize("optimize");
                backend.prepare_unit_for_emission(*output->whole);
            }

            {
                Compiler::ScopedPhase machine_code("machine code");

                if (!backend.emit_object(*output->whole, output->dest)) {
                    return false;
                }
            }
        }
    }
    else {
        for (Output *output : live) {
            isolated.push_back(make_isolated_request(
                bitcode_of(*output->whole->llvm_module), output->dest, ctx, sub,
                output->whole->optimized, timings));
            output->whole->llvm_module.reset();
            output->whole->optimized = true;
        }
    }

    if (!run_isolated_emits(isolated, jobs)) {
        return false;
    }

    for (Output &output : outputs) {
        if (output.pieces.size() != 1 || output.pieces[0] != output.dest) {
            std::string error;

            if (!join_relocatable(output.pieces, output.dest, env, error)) {
                llvm::errs() << error << "\n";
                return false;
            }
        }

        out_objects.push_back(output.dest);
    }

    return true;
}

};
