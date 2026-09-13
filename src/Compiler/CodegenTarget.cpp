#include "Compiler/CodegenTarget.h"

#include "Compiler/HostTool.h"
#include "Compiler/TargetFacts.h"

#include <llvm/TargetParser/Host.h>

#include <fmt/core.h>

#include <filesystem>

namespace Compiler
{

static constexpr const char *k_ios_min_version = "15.0";

std::string CodegenTarget::effective_triple() const
{
    return triple.empty() ? llvm::sys::getDefaultTargetTriple() : triple;
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
        // below - the phone's arch, not this machine's
        if (!arch_override.empty() && arch_override != "arm64") {
            out_error = fmt::format(
                "--ios-device is arm64; --target-arch '{}' is not a phone", arch_override);
            return false;
        }
    }

    if (facts.operating_system != "ios" || !host_is_darwin) {
        return true;
    }

    if (ios_device) {
        out_target.triple = "arm64-apple-ios";
        out_target.apple_sdk = "iphoneos";
        out_target.min_version_flag =
            std::string("-miphoneos-version-min=") + k_ios_min_version;
        return true;
    }

    const std::string arch = facts.architecture.empty() ? "arm64" : facts.architecture;
    out_target.triple = arch + "-apple-ios-simulator";
    out_target.apple_sdk = "iphonesimulator";
    out_target.min_version_flag =
        std::string("-mios-simulator-version-min=") + k_ios_min_version;

    return true;
}

void append_apple_target_args(std::vector<std::string> &argv, const CodegenTarget &target)
{
#if !defined(__APPLE__)
    (void)target;
    append_darwin_sdk_args(argv);
#else
    if (target.apple_sdk.empty()) {
        append_darwin_sdk_args(argv);
        return;
    }

    const std::filesystem::path sdk = apple_sdk_root(target.apple_sdk);
    if (!sdk.empty()) {
        argv.push_back("-isysroot");
        argv.push_back(sdk.string());
    }

    argv.push_back("-target");
    argv.push_back(target.effective_triple());
    if (!target.min_version_flag.empty()) {
        argv.push_back(target.min_version_flag);
    }
#endif
}

};
