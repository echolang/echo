#include "Compiler/LLVM/Codegen/Emit.h"

#include "Compiler/JobCount.h"
#include "Compiler/LLVM/Codegen/Backend.h"
#include "Compiler/LLVM/Codegen/Bitcode.h"
#include "Compiler/LLVM/Codegen/IsolatedEmit.h"
#include "Compiler/LLVM/Codegen/Partition.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/CompilationUnit.h"
#include "Compiler/PhaseTimings.h"
#include "Compiler/ProgressReporter.h"
#include "Compiler/TargetSubtarget.h"

#include "AST/ASTModule.h"

#include <llvm/Support/raw_ostream.h>

#include <unordered_set>

namespace Compiler::LLVM
{

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
    request.no_optimize = ctx.options.no_optimize;
    request.targeting_windows = ctx.targeting_windows();
    request.already_optimized = already_optimized;
    request.timings = timings;
    return request;
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

        for (const auto &[name, milliseconds] : results[i].slowest_isel) {
            clock.record_slowest("machine code", name, milliseconds);
        }
    }

    return true;
}

static bool partition_object_is_hit(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 0;
}

bool emit_unit_objects(
    CodegenContext &ctx,
    Backend &backend,
    const std::function<std::filesystem::path(const std::string &)> &object_for,
    std::vector<std::filesystem::path> &out_objects
)
{
    Compiler::ScopedPhase phase("emit objects");

    const unsigned jobs = Compiler::job_count();
    const Compiler::Subtarget sub = backend.subtarget();
    const bool timings = Compiler::PhaseTimings::instance().enabled();

    PartitionEnv env;
    env.triple = ctx.target_triple;
    env.cpu = sub.cpu;
    env.features = sub.features;
    env.no_optimize = ctx.options.no_optimize;
    env.targeting_windows = ctx.targeting_windows();
    env.cross = ctx.options.codegen.is_cross();

    struct Output
    {
        std::filesystem::path dest;
        std::vector<std::filesystem::path> pieces;
        CmpUnit *whole = nullptr;
    };

    std::vector<Output> outputs;
    std::vector<IsolatedEmitRequest> isolated;

    for (auto &cmp_unit : ctx.cmp_units) {
        if (!cmp_unit->llvm_module) {
            continue;
        }

        Output output;
        output.dest = object_for(cmp_unit->ast_module->name);

        const bool try_split = can_join_relocatable(env)
            && cmp_unit->ast_module != nullptr
            && cmp_unit->ast_module->name == ctx.entry_module_name
            && !cmp_unit->optimized
            && !ctx.options.debug_info;

        std::vector<UnitPartition> parts;
        if (try_split) {
            parts = partition_unit(*cmp_unit, ctx, env);
        }

        if (parts.size() >= 2) {
            std::unordered_set<std::string> seen;

            for (UnitPartition &part : parts) {
                if (!seen.insert(part.hex).second) {
                    continue;
                }

                const std::filesystem::path piece =
                    output.dest.parent_path() / ("p." + part.hex + ".o");
                output.pieces.push_back(piece);

                if (partition_object_is_hit(piece)) {
                    continue;
                }

                isolated.push_back(make_isolated_request(
                    materialize_partition(*cmp_unit, part, ctx),
                    piece, ctx, sub, false, timings));
            }

            cmp_unit->llvm_module.reset();
            cmp_unit->optimized = true;
        }
        else {
            output.pieces.push_back(output.dest);
            output.whole = cmp_unit.get();
        }

        outputs.push_back(std::move(output));
    }

    std::vector<Output *> live;
    for (Output &output : outputs) {
        if (output.whole != nullptr) {
            live.push_back(&output);
        }
    }

    // a single live module, or `ECO_JOBS=1`, stays in this LLVMContext. the bitcode
    // round-trip is the cost; run_jobs would keep one item on this thread either way
#if defined(_WIN32)
    const bool in_memory = true;
#else
    const bool in_memory = live.size() <= 1 || jobs <= 1;
#endif

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
