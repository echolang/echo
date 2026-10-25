#include "Compiler/ProfileDriver.h"

#include "Compiler/HostTool.h"
#include "Compiler/ProfileFile.h"
#include "Compiler/ProfileReport.h"
#include "Compiler/ProfileRuntime.h"
#include "Compiler/ProfileSymbolizer.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ostream>
#include <string>

std::optional<Compiler::ProfileIssue> Compiler::profile_refuse_host()
{
#if defined(_WIN32)
    return ProfileIssue {
        "Profile Unavailable",
        "'--profile' is not available on Windows in this version."
    };
#else
    const std::string clang = host_clang();
    std::error_code ec;
    if (clang.empty() || !std::filesystem::is_regular_file(clang, ec)) {
        return ProfileIssue {
            "No Clang",
            "'--profile' needs clang to compile the profiling runtime."
        };
    }

    return std::nullopt;
#endif
}

std::optional<Compiler::ProfileIssue> Compiler::profile_refuse_target(const CodegenTarget &codegen)
{
    if (codegen.is_library()) {
        return ProfileIssue {
            "Nothing To Profile",
            "'--profile' needs a program with a main, and a lib has none"
        };
    }

    if (codegen.is_wasm() || codegen.os() == TargetOs::t_ios) {
        return ProfileIssue {
            "Profile Unavailable",
            "'--profile' is for a native host program."
        };
    }

    return std::nullopt;
}

std::optional<Compiler::ProfileIssue> Compiler::profile_refuse_native_run(bool native_run)
{
    if (native_run) {
        return std::nullopt;
    }

    return ProfileIssue {
        "Profile Unavailable",
        "'--profile' needs a native run, not the JIT."
    };
}

bool Compiler::profile_link_runtime(
    const CompilerOptions &options,
    const BuildLayout &layout,
    std::vector<LinkRequirement> &objects,
    std::string &out_error
)
{
    if (!options.profiling()) {
        return true;
    }

    std::filesystem::path dir = layout.profile_dir();
    if (dir.empty()) {
        dir = layout.scratch_dir() / "profile";
    }

    std::vector<LinkRequirement> runtime;
    if (!profile_runtime_objects(options, dir, layout.scratch_dir(), runtime, out_error)) {
        return false;
    }

    objects.insert(objects.end(), runtime.begin(), runtime.end());
    return true;
}

std::filesystem::path Compiler::profile_output_path(
    const BuildLayout &layout,
    const std::string &stem
)
{
    if (const char *user_out = std::getenv("ECHO_PROFILE_OUT")) {
        if (user_out[0] != 0) {
            return user_out;
        }
    }

    const std::string used = stem.empty() ? std::string("a") : stem;
    return layout.profile_path(used);
}

void Compiler::profile_prepare_run(
    const BuildLayout &layout,
    const std::string &stem,
    std::filesystem::path &out_path,
    std::vector<std::pair<std::string, std::string>> &extra_env
)
{
    out_path = profile_output_path(layout, stem);
    std::error_code ec;
    std::filesystem::create_directories(out_path.parent_path(), ec);
    extra_env.emplace_back("ECHO_PROFILE_OUT", out_path.string());
}

std::optional<Compiler::ProfileIssue> Compiler::profile_report_run(
    const std::filesystem::path &profile_path,
    const std::filesystem::path &binary,
    std::ostream &out,
    TerminalCapabilities capabilities
)
{
    ProfileFile file;
    std::string error;
    if (!ProfileFile::open(profile_path, file, error)) {
        return ProfileIssue { "Profile Missing", error };
    }

    if (!file.symbolized()) {
        if (!ProfileSymbolizer::symbolize(file, binary, error)
            || !ProfileFile::write(profile_path, file, error)) {
            return ProfileIssue { "Profile Symbolize Failed", error };
        }
    }

    ProfileReport(out, capabilities).print_summary(file, profile_path);
    return std::nullopt;
}

int Compiler::run_profile_command(
    const DriverOptions &driver,
    std::ostream &out,
    TerminalCapabilities capabilities,
    std::optional<ProfileIssue> &out_issue
)
{
    out_issue.reset();

    if (driver.sources.empty()) {
        out_issue = ProfileIssue { "No Profile File", "'profile' needs a .ecoprof file." };
        return 1;
    }

    const std::filesystem::path path = driver.sources.front();
    ProfileFile file;
    std::string error;

    if (!ProfileFile::open(path, file, error)) {
        out_issue = ProfileIssue { "Bad Profile File", error };
        return 1;
    }

    if (!file.symbolized()) {
        const std::string binary = std::string(file.string_at(file.binary().path));
        if (binary.empty()) {
            out_issue = ProfileIssue {
                "No Binary",
                "this profile file has no binary path to symbolize against."
            };
            return 1;
        }

        ProfileBinaryIdentity identity;
        if (!ProfileSymbolizer::read_identity(binary, identity, error)) {
            out_issue = ProfileIssue { "Binary Missing", error };
            return 1;
        }

        bool uuid_present = false;
        for (unsigned b : file.binary().uuid) {
            if (b != 0) {
                uuid_present = true;
                break;
            }
        }

        if (identity.has_uuid && uuid_present
            && std::memcmp(identity.uuid.data(), file.binary().uuid, 16) != 0) {
            out_issue = ProfileIssue {
                "Binary Mismatch",
                "the binary's build id does not match this profile file."
            };
            return 1;
        }

        if (!ProfileSymbolizer::symbolize(file, binary, error)
            || !ProfileFile::write(path, file, error)) {
            out_issue = ProfileIssue { "Profile Symbolize Failed", error };
            return 1;
        }
    }

    if (driver.profile_fold.has_value()) {
        if (!ProfileReport::write_folded(file, *driver.profile_fold, out, error)) {
            out_issue = ProfileIssue { "Fold Failed", error };
            return 1;
        }
        return 0;
    }

    if (driver.profile_text) {
        ProfileReport(out, capabilities).print_text(file, path);
        return 0;
    }

    ProfileReport(out, capabilities).print_summary(file, path);
    return 0;
}
