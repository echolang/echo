#include "Compiler/CodegenTarget.h"

#include "Compiler/TargetFacts.h"

#include <llvm/TargetParser/Host.h>

#include <fmt/core.h>

namespace Compiler
{

static constexpr const char *k_ios_min_version = "15.0";
static constexpr const char *k_ios_device_arch = "arm64";

std::string CodegenTarget::effective_triple() const
{
    return triple.empty() ? llvm::sys::getDefaultTargetTriple() : triple;
}

std::string facts_architecture(bool ios_device, const std::string &arch_override)
{
    if (ios_device && arch_override.empty()) {
        return k_ios_device_arch;
    }

    return arch_override;
}

bool resolve_codegen_target(
    const TargetFacts &facts,
    bool ios_device,
    const std::string &arch_override,
    CodegenTarget &out_target,
    std::string &out_error)
{
    out_target = {};

    const bool host_is_darwin = TargetFacts::host().operating_system == "darwin";

    if (ios_device) {
        if (facts.operating_system != "ios") {
            out_error = "--ios-device needs --target-os ios";
            return false;
        }

        if (!host_is_darwin) {
            out_error = "--ios-device cross-compiles for a physical iPhone, which needs a Darwin host";
            return false;
        }

        // device is arm64; an explicit `--target-arch x86_64` would emit a phone
        // that does not exist. host x86_64 without the flag still becomes arm64
        // below - the phone's arch, not this machine's. facts_architecture is
        // the matching half, so a condition sees arm64 too
        if (!arch_override.empty() && arch_override != k_ios_device_arch) {
            out_error = fmt::format(
                "--ios-device is arm64; --target-arch '{}' is not a phone", arch_override);
            return false;
        }
    }

    if (facts.operating_system != "ios" || !host_is_darwin) {
        return true;
    }

    if (ios_device) {
        out_target.triple = std::string("arm64-apple-ios") + k_ios_min_version;
        out_target.apple_sdk = "iphoneos";
        return true;
    }

    const std::string arch = facts.architecture.empty() ? "arm64" : facts.architecture;
    out_target.triple = arch + "-apple-ios" + k_ios_min_version + "-simulator";
    out_target.apple_sdk = "iphonesimulator";

    return true;
}

};
