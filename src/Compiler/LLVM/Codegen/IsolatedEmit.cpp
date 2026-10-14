#include "Compiler/LLVM/Codegen/IsolatedEmit.h"

#include "Compiler/LLVM/Codegen/ModulePasses.h"
#include "Compiler/LLVM/Codegen/TargetMachine.h"
#include "Compiler/PhaseTimings.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Comdat.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <chrono>

namespace Compiler::LLVM
{

class ISelTimePass : public llvm::FunctionPass
{
public:
    static char ID;

    explicit ISelTimePass(std::vector<std::pair<std::string, double>> *out) :
        FunctionPass(ID), _out(out)
    {};

    bool runOnFunction(llvm::Function &function) override
    {
        const auto now = std::chrono::steady_clock::now();

        record_previous(now);
        _started = now;
        _have_previous = true;
        _previous = function.getName().str();

        return false;
    }

    ~ISelTimePass() override
    {
        record_previous(std::chrono::steady_clock::now());
    }

private:
    void record_previous(std::chrono::steady_clock::time_point now)
    {
        if (!_have_previous || _out == nullptr) {
            return;
        }

        const double milliseconds
            = std::chrono::duration<double, std::milli>(now - _started).count();
        _out->push_back({ Compiler::display_function_name(_previous), milliseconds });
    }

    std::vector<std::pair<std::string, double>> *_out = nullptr;
    bool _have_previous = false;
    std::chrono::steady_clock::time_point _started;
    std::string _previous;
};

char ISelTimePass::ID = 0;

static void assign_odr_comdats(llvm::Module &module)
{
    for (llvm::GlobalVariable &global : module.globals()) {
        if (global.hasLinkOnceODRLinkage() && !global.hasComdat()) {
            global.setComdat(module.getOrInsertComdat(global.getName()));
        }
    }

    for (llvm::Function &fn : module) {
        if (fn.hasLinkOnceODRLinkage() && !fn.hasComdat()) {
            fn.setComdat(module.getOrInsertComdat(fn.getName()));
        }
    }
}

bool emit_module_object(
    llvm::Module &module,
    llvm::TargetMachine &target_machine,
    const std::filesystem::path &object_path,
    bool targeting_windows,
    std::vector<std::pair<std::string, double>> *isel_times,
    std::string &error
)
{
    if (targeting_windows) {
        assign_odr_comdats(module);
    }

    llvm::Expected<llvm::sys::fs::TempFile> staged =
        llvm::sys::fs::TempFile::create(object_path.string() + ".tmp-%%%%%%%%");

    if (!staged) {
        error = "Could not open file: " + llvm::toString(staged.takeError());
        return false;
    }

    bool written = false;

    {
        llvm::raw_fd_ostream dest(staged->FD, false);
        llvm::legacy::PassManager pass;
        auto FileType = llvm::CodeGenFileType::ObjectFile;

        if (isel_times != nullptr) {
            pass.add(new ISelTimePass(isel_times));
        }

        if (target_machine.addPassesToEmitFile(pass, dest, nullptr, FileType)) {
            error = "TargetMachine can't emit a file of this type";
        }
        else {
            pass.run(module);
            dest.flush();

            if (dest.has_error()) {
                error = "Could not write '" + object_path.string() + "': " + dest.error().message();
                dest.clear_error();
            }
            else {
                written = true;
            }
        }
    }

    if (!written) {
        llvm::consumeError(staged->discard());
        return false;
    }

    if (llvm::Error kept = staged->keep(object_path.string())) {
        error = "Could not write '" + object_path.string() + "': " + llvm::toString(std::move(kept));
        return false;
    }

    return true;
}

IsolatedEmitResult emit_isolated_unit(const IsolatedEmitRequest &request)
{
    IsolatedEmitResult result;
    const auto started = std::chrono::steady_clock::now();

    llvm::LLVMContext context;
    // a copy, not a view: parseBitcodeFile's bitstream reader can look past the
    // payload, and getMemBuffer does not pad
    std::unique_ptr<llvm::MemoryBuffer> buffer =
        llvm::MemoryBuffer::getMemBufferCopy(request.bitcode, "unit");
    llvm::Expected<std::unique_ptr<llvm::Module>> parsed =
        llvm::parseBitcodeFile(buffer->getMemBufferRef(), context);

    if (!parsed) {
        result.ok = false;
        result.error = llvm::toString(parsed.takeError());
        return result;
    }

    std::string error;
    std::unique_ptr<llvm::TargetMachine> machine = make_target_machine(
        request.triple,
        request.cpu,
        request.features,
        request.no_optimize,
        request.pic,
        error
    );

    if (!machine) {
        result.ok = false;
        result.error = error;
        return result;
    }

    llvm::Module &module = *parsed.get();

    if (!request.already_optimized) {
        prepare_module(module, machine.get(), !request.no_optimize);
    }

    result.optimize_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    const auto isel_started = std::chrono::steady_clock::now();

    if (!emit_module_object(
            module, *machine, request.object_path, request.targeting_windows,
            request.timings ? &result.slowest_isel : nullptr, result.error)) {
        result.ok = false;
        return result;
    }

    result.isel_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - isel_started).count();
    return result;
}

};
