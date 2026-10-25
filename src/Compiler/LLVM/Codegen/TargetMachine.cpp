#include "Compiler/LLVM/Codegen/TargetMachine.h"

#include "Compiler/TargetSubtarget.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include <cassert>
#include <fmt/core.h>

llvm::TargetOptions Compiler::LLVM::target_options_for(const std::string &triple)
{
    llvm::TargetOptions opt;
    // `unreachable` is a trap, not fall-through into the next function
    opt.TrapUnreachable = true;
    const llvm::Triple parsed(triple);
    // wasm's backend miscompiles a skipped trap after noreturn. Windows SEH
    // needs the trap so .pdata still names the caller
    if (!parsed.isWasm() && !parsed.isOSWindows()) {
        opt.NoTrapAfterNoreturn = true;
    }
    return opt;
}

llvm::CodeGenOptLevel Compiler::LLVM::codegen_opt_level(Compiler::CodegenOpt opt)
{
    switch (opt) {
    case Compiler::CodegenOpt::t_none:
        return llvm::CodeGenOptLevel::None;
    case Compiler::CodegenOpt::t_baseline:
        return llvm::CodeGenOptLevel::Default;
    case Compiler::CodegenOpt::t_aggressive:
        return llvm::CodeGenOptLevel::Aggressive;
    }

    assert(false && "unhandled CodegenOpt in codegen_opt_level");
    return llvm::CodeGenOptLevel::Default;
}

std::unique_ptr<llvm::TargetMachine> Compiler::LLVM::make_target_machine(
    const std::string &triple,
    const std::string &cpu,
    const std::string &features,
    llvm::CodeGenOptLevel opt_level,
    bool pic,
    std::string &error
)
{
    Compiler::ensure_targets_for_triple(triple);

    auto *target = llvm::TargetRegistry::lookupTarget(triple, error);

    if (!target) {
        return nullptr;
    }

    // Echo's floor is `generic`; x86 MC has no such processor. llvm_cpu_name is
    // the spelling createTargetMachine will not fprintf-and-ignore
    const std::string mc_cpu = Compiler::llvm_cpu_name(triple, cpu);

    const llvm::TargetOptions opt = target_options_for(triple);
    const llvm::Reloc::Model reloc = pic ? llvm::Reloc::PIC_ : llvm::Reloc::Static;
#if LLVM_VERSION_MAJOR >= 21
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        llvm::Triple(triple), mc_cpu, features, opt, reloc, std::nullopt, opt_level));
#else
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        triple, mc_cpu, features, opt, reloc, std::nullopt, opt_level));
#endif

    if (!machine) {
        error = fmt::format("Could not create a target machine for '{}'", triple);
    }

    return machine;
}
