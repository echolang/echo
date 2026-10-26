#ifndef PROFILEDRIVER_H
#define PROFILEDRIVER_H

#pragma once

#include "Compiler/BuildLayout.h"
#include "Compiler/CodegenTarget.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/DriverOptions.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/TargetFacts.h"
#include "Compiler/TerminalCapabilities.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Compiler
{
    struct ProfileIssue
    {
        const char *title = nullptr;
        std::string message;
    };

    std::optional<ProfileIssue> profile_refuse_host();
    std::optional<ProfileIssue> profile_refuse_target(
        const CodegenTarget &codegen,
        const TargetFacts &facts
    );
    std::optional<ProfileIssue> profile_refuse_native_run(bool native_run);

    bool profile_link_runtime(
        const CompilerOptions &options,
        const BuildLayout &layout,
        std::vector<LinkRequirement> &objects,
        std::string &out_error
    );

    std::filesystem::path profile_output_path(
        const BuildLayout &layout,
        const std::string &stem
    );

    void profile_prepare_run(
        const BuildLayout &layout,
        const std::string &stem,
        std::filesystem::path &out_path,
        std::vector<std::pair<std::string, std::string>> &extra_env
    );

    std::optional<ProfileIssue> profile_report_run(
        const std::filesystem::path &profile_path,
        const std::filesystem::path &binary,
        std::ostream &out,
        TerminalCapabilities capabilities
    );

    int run_profile_command(
        const DriverOptions &driver,
        std::ostream &out,
        TerminalCapabilities capabilities,
        std::optional<ProfileIssue> &out_issue
    );
};

#endif
