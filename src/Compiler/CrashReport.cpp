#include "Compiler/CrashReport.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace
{
    // stdio rather than an iostream, and no ProgressReporter::suspend(): this may be running on the
    // spinner thread itself, and suspend() joins it. The leading newline is what keeps a live progress
    // row from swallowing the line instead
    void report_line(const std::string &what)
    {
        const std::string line = "\nechoc: internal error: " + what + "\n";
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }

    [[noreturn]] void report_uncaught_exception()
    {
        const std::exception_ptr current = std::current_exception();

        if (current == nullptr) {
            report_line("std::terminate called with no active exception");
            std::abort();
        }

        // a filesystem error names its paths separately from its sentence, and the path is the clue
        try {
            std::rethrow_exception(current);
        }
        catch (const std::filesystem::filesystem_error &e) {
            std::string what = std::string("uncaught filesystem_error: ") + e.what();

            if (!e.path1().empty()) {
                what += " [path1: '" + e.path1().generic_string() + "']";
            }

            if (!e.path2().empty()) {
                what += " [path2: '" + e.path2().generic_string() + "']";
            }

            report_line(what);
        }
        catch (const std::exception &e) {
            report_line(std::string("uncaught exception: ") + e.what());
        }
        catch (...) {
            report_line("uncaught exception of a type not derived from std::exception");
        }

        std::abort();
    }

#if defined(_WIN32)
    // the release CRT hands this nulls for every argument, so the sentence is all there is to say
    [[noreturn]] void report_invalid_parameter(
        const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
    {
        report_line("the C runtime was passed an invalid parameter");
        std::abort();
    }
#endif
}

void Compiler::install_crash_reporting()
{
    std::set_terminate(report_uncaught_exception);

#if defined(_WIN32)
    _set_invalid_parameter_handler(report_invalid_parameter);
#endif
}
