#ifndef COVERAGEDRIVER_H
#define COVERAGEDRIVER_H

#pragma once

#include "Compiler/BuildLayout.h"
#include "Compiler/CompilerOptions.h"
#include "Compiler/LinkRequirement.h"
#include "Compiler/TestSelection.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Compiler
{
    enum class CoverageSiteKind
    {
        t_fn,
        t_line,
        t_branch
    };

    const char *coverage_site_kind_name(CoverageSiteKind kind);

    // one counter the runtime will remap. identity is (module, local id)
    struct CoverageSite
    {
        uint32_t id = 0;
        std::string module;
        std::string dir;
        std::string file;
        uint32_t line = 0;
        uint32_t column = 0;
        CoverageSiteKind kind = CoverageSiteKind::t_line;
        std::string edge;
        std::string function;
    };

    typedef std::map<std::string, std::vector<CoverageSite>> CoverageModuleSites;

    // object-filename stem for a module: sanitized bytes plus a short digest of the
    // original name, so two modules that collapse to the same letters stay distinct
    std::string coverage_module_stem(std::string_view name);

    bool coverage_link_runtime(
        const CompilerOptions &options,
        const BuildLayout &layout,
        std::vector<LinkRequirement> &objects,
        std::string &out_error
    );

    std::filesystem::path coverage_map_path(const BuildLayout &layout);
    std::filesystem::path coverage_test_path(const BuildLayout &layout, const TestCase &test);

    // writes this pass's module sidecars, keeps cached modules' sidecars, and merges
    // them into map.json. `cached_modules` are names this frontend did not emit
    bool write_coverage_map(
        const BuildLayout &layout,
        const CoverageModuleSites &compiled,
        const std::set<std::string> &cached_modules,
        std::string &out_error
    );

    void coverage_prepare_test(
        const BuildLayout &layout,
        const TestCase &test,
        std::vector<std::pair<std::string, std::string>> &extra_env
    );

    void coverage_report_run(const BuildLayout &layout, std::ostream &out);
};

#endif
