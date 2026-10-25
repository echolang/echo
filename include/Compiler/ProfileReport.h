#ifndef PROFILEREPORT_H
#define PROFILEREPORT_H

#pragma once

#include "Compiler/CommandLineOption.h"
#include "Compiler/ProfileFile.h"
#include "Compiler/TerminalCapabilities.h"

#include <filesystem>
#include <iosfwd>
#include <string>

namespace Compiler
{
    // glyphs for the human report. colour is TerminalCapabilities, asked per span
    struct ProfileReportTheme
    {
        const char *arrow;
        const char *dot;
        bool chips;

        static ProfileReportTheme pretty();
        static ProfileReportTheme ascii();
    };

    // the human summary `echoc run --profile` prints, and `echoc profile --text` / `--folded`
    class ProfileReport
    {
    public:
        ProfileReport(std::ostream &out, TerminalCapabilities capabilities);

        void print_summary(const ProfileFile &file, const std::filesystem::path &path);
        void print_text(const ProfileFile &file, const std::filesystem::path &path);

        static bool write_folded(
            const ProfileFile &file,
            ProfileFoldKind kind,
            std::ostream &out,
            std::string &out_error
        );

    private:
        std::ostream &_out;
        TerminalCapabilities _capabilities;
        ProfileReportTheme _theme;

        void write_report(
            const ProfileFile &file,
            const std::filesystem::path &path,
            size_t rows,
            bool extra
        );
    };
};

#endif
