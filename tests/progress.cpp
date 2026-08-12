#include <catch2/catch_test_macros.hpp>

#include <Compiler/ProgressReporter.h>
#include <Compiler/TerminalCapabilities.h>

#include <cctype>
#include <sstream>
#include <string>

#include "subprocess.h"
#include "terminal_fixture.h"

// what the e2e corpus cannot assert, and for the opposite reason to tests/diagnostics.cpp: the corpus
// pins what a *pipe* gets, and a pipe gets nothing at all from this - which is the property the whole
// design rests on and is asserted by name at the bottom of this file.
//
// everything above it hands the reporter an ostringstream, a forced TerminalCapabilities and literal
// milliseconds, so a row is byte-comparable. That is why Compiler::ProgressReporter owns no clock

using Compiler::ProgressPhase;
using Compiler::ProgressReporter;
using Compiler::ProgressState;
using Compiler::TerminalCapabilities;
using EchoTests::a_terminal;

namespace
{
    constexpr const char *ERASE = "\r\x1b[K";
};

TEST_CASE("a disabled reporter writes nothing at all", "[progress]")
{
    // **the case the 596 e2e goldens rest on.** Every entry point is called on a default-constructed
    // reporter, and the whole of what may reach a stream is nothing
    std::ostringstream out;
    ProgressReporter reporter;

    REQUIRE(!reporter.enabled());

    reporter.open(ProgressPhase::t_parse, "stdlib");
    reporter.tick("array.eco");
    reporter.set_detail("21 files");
    reporter.commit(ProgressState::t_done, 182, { "a.eco", "b.eco" });
    reporter.row(ProgressPhase::t_cached, "stdlib", "reused", ProgressState::t_skipped);
    reporter.suspend();
    reporter.close("compiled 'main'", 343);

    REQUIRE(out.str() == "");

    // and the singleton is one of those, until main() says otherwise - which is what makes the tick in
    // the lexer's file loop free for every other suite in this binary
    REQUIRE(!ProgressReporter::instance().enabled());
}

TEST_CASE("the live row is rewritten in place, one frame per redraw", "[progress]")
{
    std::ostringstream out;
    ProgressReporter reporter(out, a_terminal());

    reporter.open(ProgressPhase::t_parse, "stdlib");
    reporter.tick("array.eco");
    reporter.tick("string.eco");

    const std::string expected
        = std::string(ERASE) + "  ⠋  parse            stdlib"
        + ERASE + "  ⠙  parse            stdlib        array.eco"
        + ERASE + "  ⠹  parse            stdlib        string.eco";

    REQUIRE(out.str() == expected);

    // nothing above the live line was written, so nothing above it can have been disturbed
    REQUIRE(out.str().find('\n') == std::string::npos);
}

TEST_CASE("a committed row ends the line and lists its files under it", "[progress]")
{
    std::ostringstream out;
    ProgressReporter reporter(out, a_terminal());

    reporter.open(ProgressPhase::t_parse, "main");
    reporter.set_detail("2 files");
    reporter.commit(ProgressState::t_done, 4, { "src/main.eco", "src/scene.eco" });

    REQUIRE(out.str() ==
        std::string(ERASE) + "  ⠋  parse            main"
        + ERASE + "  ✓  parse            main          2 files          4 ms\n"
        + "       src/main.eco\n"
        + "       src/scene.eco\n");
}

TEST_CASE("a failed row draws the failure mark and keeps its files to itself", "[progress]")
{
    std::ostringstream out;
    ProgressReporter reporter(out, a_terminal());

    reporter.open(ProgressPhase::t_semantic_passes, "");
    reporter.commit(ProgressState::t_failed, 38, { "src/main.eco" });

    // a step that failed did not do the work its file list describes, so the list is not written. The
    // diagnostic that follows on this same stream is what the reader needs next
    REQUIRE(out.str() ==
        std::string(ERASE) + "  ⠋  semantic passes"
        + ERASE + "  ✗  semantic passes                                38 ms\n");
}

TEST_CASE("suspend erases the row, is idempotent, and needs no resume", "[progress]")
{
    std::ostringstream out;
    ProgressReporter reporter(out, a_terminal());

    reporter.open(ProgressPhase::t_cc, "gfx");
    const size_t after_open = out.str().size();

    reporter.suspend();
    REQUIRE(out.str().substr(after_open) == ERASE);

    // the second call writes nothing: there is a row to redraw and nothing drawn, which is the state a
    // subprocess is about to write into
    reporter.suspend();
    reporter.suspend();
    REQUIRE(out.str().substr(after_open) == ERASE);

    // and the row comes back with no caller co-operation
    reporter.tick("shim.c");
    REQUIRE(out.str().substr(after_open) == std::string(ERASE) + ERASE + "  ⠙  cc               gfx           shim.c");
}

TEST_CASE("a commit writes the same bytes whether or not something suspended", "[progress]")
{
    auto commit_bytes = [](bool suspend_first) {
        std::ostringstream out;
        ProgressReporter reporter(out, a_terminal());

        reporter.open(ProgressPhase::t_codegen, "");
        if (suspend_first) {
            reporter.suspend();
        }

        const size_t before = out.str().size();
        reporter.commit(ProgressState::t_done, 204);

        return out.str().substr(before);
    };

    // the erase is unconditional on a commit for exactly this reason: what a row looks like must not
    // depend on whether clang happened to run underneath it
    REQUIRE(commit_bytes(false) == commit_bytes(true));
}

TEST_CASE("a standalone row does not disturb a live one", "[progress]")
{
    std::ostringstream out;
    ProgressReporter reporter(out, a_terminal());

    reporter.open(ProgressPhase::t_codegen, "");
    const size_t before = out.str().size();

    reporter.row(ProgressPhase::t_cached, "stdlib", "reused", ProgressState::t_skipped);

    REQUIRE(out.str().substr(before) ==
        std::string(ERASE) + ERASE + "  ·  cached           stdlib        reused\n"
        + ERASE + "  ⠋  codegen");
}

TEST_CASE("both themes draw the same information", "[progress]")
{
    auto rows = [](bool unicode) {
        std::ostringstream out;
        ProgressReporter reporter(out, a_terminal(unicode));

        reporter.open(ProgressPhase::t_emit, "app");
        reporter.set_detail("1 module");
        reporter.commit(ProgressState::t_done, 88);
        reporter.row(ProgressPhase::t_cached, "stdlib", "reused", ProgressState::t_skipped);
        reporter.close("built 'app'", 651);

        return out.str();
    };

    const std::string pretty = rows(true);
    const std::string ascii = rows(false);

    REQUIRE(pretty != ascii);

    // the glyphs differ and nothing else does - the same words, the same columns, the same numbers
    for (const char *word : { "emit + link", "app", "1 module", "88 ms", "cached", "reused", "built 'app' in 651 ms" }) {
        REQUIRE(pretty.find(word) != std::string::npos);
        REQUIRE(ascii.find(word) != std::string::npos);
    }

    // and the redraw is on in both: cursor movement is an `interactive` question, not a glyph one
    REQUIRE(ascii.find(ERASE) != std::string::npos);
    REQUIRE(ascii.find("  +  emit + link      app           1 module        88 ms\n") != std::string::npos);
    REQUIRE(ascii.find("  -  cached           stdlib        reused\n") != std::string::npos);
}

TEST_CASE("no row ever reaches the last column of the terminal", "[progress]")
{
    // the erase clears to the end of the *physical* line, so a row that wrapped leaves half of the
    // previous frame on screen forever. This is the invariant that keeps that from happening
    const std::string very_long_path
        = "modules/rendering/backends/vulkan/pipeline/descriptors/very_long_file_name.eco";

    for (const unsigned int width : { 0u, 20u, 40u, 60u, 80u, 120u }) {
        for (const bool unicode : { true, false }) {
            std::ostringstream out;
            ProgressReporter reporter(out, a_terminal(unicode, width));

            reporter.open(ProgressPhase::t_semantic_passes, "rendering");
            reporter.tick(very_long_path);
            reporter.commit(ProgressState::t_done, 123456);

            const unsigned int limit = width > 0 ? width : 80;

            // walked as a terminal would: an escape sequence occupies no column, a carriage return ends
            // a frame as much as a newline does, and a UTF-8 continuation byte is not a glyph
            const std::string drawn = out.str();
            size_t columns = 0;

            for (size_t i = 0; i < drawn.size(); i++) {
                if (drawn[i] == '\x1b') {
                    while (i < drawn.size() && !std::isalpha(static_cast<unsigned char>(drawn[i]))) {
                        i++;
                    }
                    continue;
                }

                if (drawn[i] == '\n' || drawn[i] == '\r') {
                    columns = 0;
                    continue;
                }

                if ((static_cast<unsigned char>(drawn[i]) & 0xC0) != 0x80) {
                    columns++;
                }

                REQUIRE(columns < limit);
            }
        }
    }
}

TEST_CASE("every phase has a word", "[progress]")
{
    // the switch has no default, so a phase added without a label does not compile. This pins that the
    // labels are PhaseTimings' spellings, which is what keeps `-t` and the checklist one vocabulary
    using Compiler::progress_phase_label;

    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_parse)) == "parse");
    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_semantic_passes)) == "semantic passes");
    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_cached)) == "cached");
    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_cc)) == "cc");
    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_codegen)) == "codegen");
    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_optimize)) == "optimize");
    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_emit)) == "emit + link");
    REQUIRE(std::string(progress_phase_label(ProgressPhase::t_test)) == "test");
}

// **the closing line says the outcome of what it closed, not of the closing.** A compile that got as far as
// this line succeeded by definition, which is why the mark defaults - but a test run can do everything it was
// asked and still have failed, and signing that with a `✓` would make the summary disagree with the rows
// above it
TEST_CASE("the closing line can report a failure", "[progress]")
{
    std::ostringstream done;
    ProgressReporter(done, a_terminal()).close("3 tests passed", 12);

    std::ostringstream failed;
    ProgressReporter(failed, a_terminal()).close("3 tests, 1 failed", 12, ProgressState::t_failed);

    REQUIRE(done.str() == "\n  ✓  3 tests passed in 12 ms\n\n");
    REQUIRE(failed.str() == "\n  ✗  3 tests, 1 failed in 12 ms\n\n");
}

TEST_CASE("colour wraps the mark and nothing that is measured", "[progress]")
{
    std::ostringstream out;
    ProgressReporter reporter(out, a_terminal(true, 80, /*color=*/true));

    reporter.open(ProgressPhase::t_parse, "main");
    reporter.set_detail("1 file");
    reporter.commit(ProgressState::t_done, 4);

    // an SGR sequence has no width, so it is applied after the columns are laid out - the row's own text
    // is the same one the uncoloured case produces
    REQUIRE(out.str().find("\x1b[1;32m✓\x1b[0m  parse            main          1 file           4 ms\n")
        != std::string::npos);
}

TEST_CASE("a compile through a pipe writes no cursor movement", "[progress]")
{
    // the corpus proves this 596 times over. It is named here so the next person reads the gate as a
    // deliberate refusal rather than an oversight - there is no --progress=always, and the reason is
    // this assertion
    EchoTests::ScopedProject project("progress", "piped_output_is_plain");
    EchoTests::write_file(project.root() / "main.eco", "echo \"hello\";\n");

    const EchoTests::ProcessResult plain = project.echoc("run main.eco");
    REQUIRE(plain.exit_code == 0);
    REQUIRE(plain.output.find('\r') == std::string::npos);
    REQUIRE(plain.output.find('\x1b') == std::string::npos);

    // and --silent changes nothing here, because there was nothing to silence
    const EchoTests::ProcessResult silent = project.echoc("run --silent main.eco");
    REQUIRE(silent.exit_code == 0);
    REQUIRE(silent.output == plain.output);
}
