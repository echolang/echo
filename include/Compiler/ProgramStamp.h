#ifndef PROGRAMSTAMP_H
#define PROGRAMSTAMP_H

#pragma once

#include "Compiler/BuildLayout.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/ModuleCache.h"
#include "Compiler/TargetFacts.h"
#include "Parser/ManifestParser.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Compiler
{
    // skip plan: the driver is plan, skip or build, write. it does not assemble keys itself
    struct ProgramStampPlan
    {
        std::filesystem::path path;
        std::vector<const Parser::ModuleManifest *> manifests;
        CompilerOptions options;
        std::vector<LinkRequirement> link;
        uint64_t echo_key = 0;

        // Echo key inputs, handed to a build that proceeds
        std::map<std::string, ModuleCacheKey> module_keys;

        Parser::ActiveTargets active_targets;
        std::set<std::string> test_modules;
        bool optimize_whole = false;
        std::string program_name;
        std::filesystem::path entry_file;
        std::filesystem::path output;
    };

    // reverse-dependency order, command line last so a manifest wins the dedup. from the manifests:
    // a stamp asks before there is a FrontEnd
    bool collect_program_link(
        const std::vector<const Parser::ModuleManifest *> &manifests,
        const Parser::ActiveTargets &active_targets,
        const std::vector<std::string> &command_line,
        const TargetFacts &facts,
        std::vector<LinkRequirement> &out,
        std::string &out_error
    );

    // seated with this program's codegen row: the same spelling the front end gets
    struct ProgramStampRequest
    {
        const std::vector<Parser::ModuleManifest> &all_manifests;
        const std::string &entry_module;
        const Parser::ActiveTargets &active_targets;
        CompilerOptions options;
        const std::set<std::string> &test_modules;
        bool optimize_whole = false;
        const std::string &program_name;
        const std::filesystem::path &entry_file;
        const std::filesystem::path &output;
        const BuildLayout &layout;
        const std::vector<std::string> &command_line_link;
        const TargetFacts &facts;

        // a dump or a loose `.eco` must actually run: a skip prints nothing
        bool loose_sources = false;
        bool dumps = false;

        bool can_stamp() const {
            return !loose_sources && !dumps;
        }
    };

    // the stamp, or nullopt when none can speak. an unreadable source is no stamp, not a refusal
    bool plan_program_stamp(
        const ProgramStampRequest &request,
        std::optional<ProgramStampPlan> &out,
        std::string &out_error
    );

    bool program_stamp_is_current(const ProgramStampPlan &plan, const BuildLayout &layout);

    // after the link. a probe that cannot run leaves no record
    void write_built_program_stamp(const ProgramStampPlan &plan, const BuildLayout &layout);
};

#endif

