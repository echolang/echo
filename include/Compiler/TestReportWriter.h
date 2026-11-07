#ifndef TESTREPORTWRITER_H
#define TESTREPORTWRITER_H

#pragma once

#include "Compiler/TestRunner.h"

#include <chrono>
#include <ostream>

namespace Compiler
{
    // JSON Lines sink for `echoc test --report`, schema v2. a third medium, not a
    // mode of TestReporter: the listing stays a listing, and this is written from
    // the same TestResult the listing already received. v2 adds optional `failure`
    class TestReportWriter
    {
    public:
        explicit TestReportWriter(std::ostream &out);

        void begin(size_t total);
        void result(const TestResult &result);
        void finish();

    private:
        std::ostream &_out;
        size_t _total = 0;
        size_t _failed = 0;
        std::chrono::steady_clock::time_point _started;

        void write_line(const std::string &line);
    };
};

#endif
