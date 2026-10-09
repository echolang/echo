#include "Compiler/LLVM/Codegen/TargetMachine.h"

#include "Compiler/TargetSubtarget.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>

std::unique_ptr<llvm::TargetMachine> Compiler::LLVM::make_target_machine(
    const std::string &triple,
    const std::string &cpu,
    const std::string &features,
    bool no_optimize,
    bool pic,
    std::string &error
)
{
    Compiler::ensure_native_target_registered();

    auto *target = llvm::TargetRegistry::lookupTarget(triple, error);

    if (!target) {
        return nullptr;
    }

    const llvm::CodeGenOptLevel opt_level =
        no_optimize ? llvm::CodeGenOptLevel::None : llvm::CodeGenOptLevel::Default;

    llvm::TargetOptions opt;
    const llvm::Reloc::Model reloc = pic ? llvm::Reloc::PIC_ : llvm::Reloc::Static;
#if LLVM_VERSION_MAJOR >= 21
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        llvm::Triple(triple), cpu, features, opt, reloc, std::nullopt, opt_level));
#else
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        triple, cpu, features, opt, reloc, std::nullopt, opt_level));
#endif

    if (!machine) {
        error = fmt::format("Could not create a target machine for '{}'", triple);
    }

    return machine;
}
