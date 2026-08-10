#include "Compiler/TargetFacts.h"

#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cassert>

namespace Compiler
{

namespace
{
    // the vocabularies, spelled once. a condition's value is checked against these, an override on the
    // command line is checked against these, and the "expected one of" in both messages is built from
    // these - so a platform added here becomes sayable everywhere at once
    const std::vector<std::string> k_operating_systems = { "darwin", "linux", "windows" };
    const std::vector<std::string> k_architectures = { "arm64", "x86_64" };

    // the two axes a condition may name, spelled beside the vocabularies they select
    constexpr const char *k_axis_os = "os";
    constexpr const char *k_axis_arch = "arch";

    // LLVM's triple vocabulary is not ours, and the mapping is the point of this function: `macosx` and
    // `ios` are both Darwin as far as which libc calls exist, and `aarch64` is what LLVM calls the
    // architecture Apple calls `arm64`. Echo's names are the ones a person would write
    std::string operating_system_of(const llvm::Triple &triple)
    {
        if (triple.isOSDarwin()) {
            return "darwin";
        }

        if (triple.isOSLinux()) {
            return "linux";
        }

        if (triple.isOSWindows()) {
            return "windows";
        }

        return "";
    }

    std::string architecture_of(const llvm::Triple &triple)
    {
        switch (triple.getArch()) {
            case llvm::Triple::aarch64:
            case llvm::Triple::aarch64_be:
                return "arm64";

            case llvm::Triple::x86_64:
                return "x86_64";

            default:
                return "";
        }
    }
};

const std::vector<std::string> &TargetFacts::known_operating_systems()
{
    return k_operating_systems;
}

const std::vector<std::string> &TargetFacts::known_architectures()
{
    return k_architectures;
}

bool TargetFacts::is_known_operating_system(const std::string &name)
{
    return std::find(k_operating_systems.begin(), k_operating_systems.end(), name)
        != k_operating_systems.end();
}

bool TargetFacts::is_known_architecture(const std::string &name)
{
    return std::find(k_architectures.begin(), k_architectures.end(), name) != k_architectures.end();
}

bool TargetFacts::is_axis(const std::string &name)
{
    return name == k_axis_os || name == k_axis_arch;
}

std::string TargetFacts::axis_names()
{
    return fmt::format("{}, {}", k_axis_os, k_axis_arch);
}

bool TargetFacts::axis_equals(
    const std::string &axis,
    const std::string &value,
    bool &out_match,
    std::string &out_error
) const
{
    assert(is_axis(axis) && "axis_equals called with a name is_axis rejected");

    const bool is_os = axis == k_axis_os;

    if (!(is_os ? is_known_operating_system(value) : is_known_architecture(value))) {
        out_error = fmt::format("unknown {} '{}', expected one of: {}", axis, value,
            fmt::join(is_os ? k_operating_systems : k_architectures, ", "));
        return false;
    }

    out_match = value == (is_os ? operating_system : architecture);
    return true;
}

bool TargetFacts::resolve(
    const std::string &os_override,
    const std::string &arch_override,
    const std::vector<std::string> &defines,
    TargetFacts &out_facts,
    std::string &out_error
)
{
    const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());

    out_facts.operating_system = operating_system_of(triple);
    out_facts.architecture = architecture_of(triple);

    // **an unrecognised host is not an error.** It leaves the axis empty, so `os == darwin` is simply
    // false and an `#[else]` arm is what such a platform gets - which is the right outcome for a target
    // nobody has taught this compiler about yet. Refusing to compile at all would be worse, and a
    // *condition* naming an unknown value is still caught, because that is a typo rather than a platform
    if (!os_override.empty()) {
        if (!is_known_operating_system(os_override)) {
            out_error = fmt::format(
                "unknown --target-os '{}', expected one of: {}",
                os_override, fmt::join(k_operating_systems, ", "));
            return false;
        }

        out_facts.operating_system = os_override;
    }

    if (!arch_override.empty()) {
        if (!is_known_architecture(arch_override)) {
            out_error = fmt::format(
                "unknown --target-arch '{}', expected one of: {}",
                arch_override, fmt::join(k_architectures, ", "));
            return false;
        }

        out_facts.architecture = arch_override;
    }

    for (const std::string &define : defines) {
        // a define carrying a value would be a substitution, and this compiler has `const` declarations
        // for naming a value. refused rather than accepted-and-ignored, so `--define N=1` cannot look
        // like it worked
        if (define.find('=') != std::string::npos) {
            out_error = fmt::format(
                "--define takes a bare name, not '{}' - a define is a flag a condition can test, "
                "and carries no value", define);
            return false;
        }

        if (define.empty()) {
            out_error = "--define needs a name";
            return false;
        }

        // reserved against the axis names, so `--define os` cannot shadow the thing `os == darwin` reads.
        // The condition grammar would never look a define up under those names, but a person who wrote it
        // believed something false and should be told
        if (is_axis(define)) {
            out_error = fmt::format(
                "'{}' is a condition axis, not a flag - write `#[if: {} == <value>]` rather than "
                "defining it", define, define);
            return false;
        }

        out_facts.defines.insert(define);
    }

    return true;
}

TargetFacts TargetFacts::host()
{
    TargetFacts facts;
    std::string error;

    // there are no overrides, and every failure above is an override outside a vocabulary
    const bool resolved = TargetFacts::resolve("", "", {}, facts, error);
    assert(resolved && "resolving the host's facts cannot fail");
    (void)resolved;

    return facts;
}

bool TargetFacts::has_define(const std::string &name) const
{
    return defines.find(name) != defines.end();
}

std::string TargetFacts::cache_signature() const
{
    std::string result = "os=" + operating_system + ";arch=" + architecture + ";";

    // the set is ordered, so this is stable across two invocations that typed the flags differently
    for (const std::string &define : defines) {
        result += "define=" + define + ";";
    }

    return result;
}

std::string TargetFacts::shared_library_extension() const
{
    if (operating_system == "darwin") {
        return ".dylib";
    }

    if (operating_system == "windows") {
        return ".dll";
    }

    return ".so";
}

};
