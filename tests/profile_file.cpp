#include <catch2/catch_test_macros.hpp>

#include <Compiler/ProfileFile.h>
#include <Compiler/ProfileReport.h>
#include <Compiler/TerminalCapabilities.h>

#include <eco_profile.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "subprocess.h"
#include "terminal_fixture.h"

// in-process .ecoprof codec tests, plus `echoc profile` on synthetic files.
// native `echoc run --profile` lives in tests/profile.cpp: those need a host sampler

using Compiler::ProfileFileBuilder;
using Compiler::ProfileReport;
using Compiler::TerminalCapabilities;
using EchoTests::a_terminal;
using EchoTests::ProcessResult;
using EchoTests::ScopedProject;

namespace
{
    void put_u32(std::vector<unsigned char> &out, uint32_t v)
    {
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    }

    void put_u64(std::vector<unsigned char> &out, uint64_t v)
    {
        unsigned char b[8];
        std::memcpy(b, &v, 8);
        out.insert(out.end(), b, b + 8);
    }

    void put_section(
        std::vector<unsigned char> &out,
        uint32_t tag,
        const std::vector<unsigned char> &payload
    )
    {
        put_u32(out, tag);
        put_u32(out, 0);
        put_u64(out, payload.size());
        out.insert(out.end(), payload.begin(), payload.end());
        while ((out.size() & 3u) != 0) {
            out.push_back(0);
        }
    }

    std::vector<unsigned char> empty_header()
    {
        std::vector<unsigned char> header(64, 0);
        std::memcpy(header.data(), ECO_PROF_MAGIC, 8);
        uint32_t version = ECO_PROF_VERSION;
        uint32_t header_bytes = ECO_PROF_HEADER_BYTES;
        std::memcpy(header.data() + 8, &version, 4);
        std::memcpy(header.data() + 12, &header_bytes, 4);
        return header;
    }
}

TEST_CASE("a synthetic profile file round-trips", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.header().ticks_per_second = 1000000000ull;
    b.header().sample_hz = 1000;
    b.header().thread_count = 1;
    b.intern_string("");
    b.set_frames({ 0x1000, 0x2000 });
    b.set_stacks({ { 0, 2 } });
    Compiler::ProfileEvent event;
    event.site_or_kind = ECO_PROF_KIND_ALLOC;
    event.stack = 0;
    event.count = 3;
    event.bytes = 24;
    b.add_event(event);
    Compiler::ProfileFile file = std::move(b).build();

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "echo-profile-roundtrip.ecoprof";
    std::string error;
    REQUIRE(Compiler::ProfileFile::write(path, file, error));

    Compiler::ProfileFile back;
    REQUIRE(Compiler::ProfileFile::open(path, back, error));
    REQUIRE(back.header().sample_hz == 1000);
    REQUIRE(back.frames().size() == 2);
    REQUIRE(back.events().size() == 1);
    REQUIRE(back.events()[0].count == 3);
    REQUIRE(back.events()[0].bytes == 24);
    std::filesystem::remove(path);
}

TEST_CASE("top allocation paths by bytes sort by bytes", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.intern_string("");
    b.set_frames({ 0x1000, 0x2000 });
    b.set_stacks({ { 0, 1 }, { 1, 1 } });

    Compiler::ProfileEvent small;
    small.site_or_kind = ECO_PROF_KIND_ALLOC;
    small.stack = 0;
    small.count = 100;
    small.bytes = 8;
    b.add_event(small);

    Compiler::ProfileEvent large;
    large.site_or_kind = ECO_PROF_KIND_ALLOC;
    large.stack = 1;
    large.count = 1;
    large.bytes = 1'000'000;
    b.add_event(large);
    Compiler::ProfileFile file = std::move(b).build();

    std::ostringstream out;
    ProfileReport(out, TerminalCapabilities::plain()).print_summary(file, "app.ecoprof");
    const std::string text = out.str();
    const size_t bytes_title = text.find("Allocations (bytes)");
    REQUIRE(bytes_title != std::string::npos);
    const size_t first = text.find("0x2000", bytes_title);
    const size_t second = text.find("0x1000", bytes_title);
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    REQUIRE(first < second);
}

TEST_CASE("the summary is pretty on a terminal and ascii in a pipe", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.header().sample_hz = 1000;
    b.intern_string("");
    b.set_frames({ 0x10, 0x20 });
    b.set_stacks({ { 0, 2 } });

    Compiler::ProfileEvent alloc;
    alloc.site_or_kind = ECO_PROF_KIND_ALLOC;
    alloc.stack = 0;
    alloc.count = 3;
    alloc.bytes = 24;
    b.add_event(alloc);

    Compiler::ProfileSample sample;
    sample.stack = 0;
    sample.samples = 8;
    b.add_sample(sample);
    Compiler::ProfileFile file = std::move(b).build();

    auto render = [&](TerminalCapabilities caps) {
        std::ostringstream out;
        ProfileReport(out, caps).print_summary(file, "app.ecoprof");
        return out.str();
    };

    const std::string ascii = render(TerminalCapabilities::plain());
    const std::string pretty = render(a_terminal(true));
    const std::string colour = render(a_terminal(true, 80, true));

    for (const char *word : { "app.ecoprof", "3 allocs", "24 B", "Hottest (self)", "Allocations (count)" }) {
        REQUIRE(ascii.find(word) != std::string::npos);
        REQUIRE(pretty.find(word) != std::string::npos);
        REQUIRE(colour.find(word) != std::string::npos);
    }

    REQUIRE(ascii.find("[profile]") != std::string::npos);
    REQUIRE(ascii.find(" -> ") != std::string::npos);
    REQUIRE(ascii.find("\x1b[") == std::string::npos);
    REQUIRE(ascii.find("PROFILE") == std::string::npos);

    REQUIRE(pretty.find("[profile]") == std::string::npos);
    REQUIRE(pretty.find("PROFILE") != std::string::npos);
    REQUIRE(pretty.find(" → ") != std::string::npos);
    REQUIRE(pretty.find("\x1b[") == std::string::npos);

    REQUIRE(colour.find("\x1b[") != std::string::npos);
    REQUIRE(colour.find("PROFILE") != std::string::npos);
}

TEST_CASE("runtime leaf frames fold out of collapsed paths", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.intern_string("");
    const uint32_t work = b.intern_string("work");
    const uint32_t thunk = b.intern_string("__eco_prof_realloc");
    b.set_frames({ 0x2000, 0x1000 });
    b.set_stacks({ { 0, 2 } });

    Compiler::ProfileInline leaf;
    leaf.function = thunk;
    b.add_inline(leaf);
    Compiler::ProfileInline caller;
    caller.function = work;
    b.add_inline(caller);
    b.set_addrs({ { 0, 1 }, { 1, 1 } });
    b.mark_symbolized();

    Compiler::ProfileEvent event;
    event.site_or_kind = ECO_PROF_KIND_ALLOC;
    event.stack = 0;
    event.count = 1;
    event.bytes = 16;
    b.add_event(event);
    Compiler::ProfileFile file = std::move(b).build();

    std::ostringstream folded;
    std::string error;
    REQUIRE(Compiler::ProfileReport::write_folded(
        file, Compiler::ProfileFoldKind::t_allocs, folded, error));
    REQUIRE(folded.str().find("work 1") != std::string::npos);
    REQUIRE(folded.str().find("__eco_") == std::string::npos);

    std::ostringstream summary;
    ProfileReport(summary, TerminalCapabilities::plain()).print_summary(file, "app.ecoprof");
    REQUIRE(summary.str().find("work") != std::string::npos);
    REQUIRE(summary.str().find("__eco_") == std::string::npos);
}

TEST_CASE("the summary shows five rows and --text shows ten", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.intern_string("");
    b.set_frames({ 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000 });
    b.set_stacks({ { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 1 } });

    for (uint32_t i = 0; i < 6; i++) {
        Compiler::ProfileEvent event;
        event.site_or_kind = ECO_PROF_KIND_ALLOC;
        event.stack = i;
        event.count = 6 - i;
        event.bytes = 8;
        b.add_event(event);
    }
    Compiler::ProfileFile file = std::move(b).build();

    std::ostringstream summary;
    ProfileReport(summary, TerminalCapabilities::plain()).print_summary(file, "app.ecoprof");
    const std::string summary_text = summary.str();
    const size_t count_title = summary_text.find("Allocations (count)");
    REQUIRE(count_title != std::string::npos);
    REQUIRE(summary_text.find("0x1000", count_title) != std::string::npos);
    REQUIRE(summary_text.find("0x5000", count_title) != std::string::npos);
    REQUIRE(summary_text.find("0x6000", count_title) == std::string::npos);

    std::ostringstream text;
    ProfileReport(text, TerminalCapabilities::plain()).print_text(file, "app.ecoprof");
    REQUIRE(text.str().find("0x6000") != std::string::npos);
    REQUIRE(text.str().find("stacks") != std::string::npos);
}

TEST_CASE("an unknown tag is skipped", "[profile]")
{
    std::vector<unsigned char> bytes(64 + 16, 0);
    std::memcpy(bytes.data(), ECO_PROF_MAGIC, 8);
    uint32_t version = ECO_PROF_VERSION;
    uint32_t header_bytes = ECO_PROF_HEADER_BYTES;
    std::memcpy(bytes.data() + 8, &version, 4);
    std::memcpy(bytes.data() + 12, &header_bytes, 4);
    uint32_t tag = ECO_PROF_TAG('Z', 'Z', 'Z', 'Z');
    uint64_t payload = 0;
    std::memcpy(bytes.data() + 64, &tag, 4);
    std::memcpy(bytes.data() + 72, &payload, 8);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "echo-profile-unknown.ecoprof";
    EchoTests::write_file(path, std::string(reinterpret_cast<char *>(bytes.data()), bytes.size()));

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(path, file, error));
    std::filesystem::remove(path);
}

TEST_CASE("a truncated or future-version file is refused", "[profile]")
{
    Compiler::ProfileFile file;
    std::string error;
    REQUIRE_FALSE(Compiler::ProfileFile::open("/no/such/profile.ecoprof", file, error));
    REQUIRE(error.find("could not be read") != std::string::npos);

    std::vector<unsigned char> bytes(64, 0);
    std::memcpy(bytes.data(), ECO_PROF_MAGIC, 8);
    uint32_t version = ECO_PROF_VERSION + 1;
    uint32_t header_bytes = ECO_PROF_HEADER_BYTES;
    std::memcpy(bytes.data() + 8, &version, 4);
    std::memcpy(bytes.data() + 12, &header_bytes, 4);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "echo-profile-future.ecoprof";
    EchoTests::write_file(path, std::string(reinterpret_cast<char *>(bytes.data()), bytes.size()));
    REQUIRE_FALSE(Compiler::ProfileFile::open(path, file, error));
    REQUIRE(error.find("newer") != std::string::npos);
    std::filesystem::remove(path);

    std::vector<unsigned char> short_bytes(8, 0);
    std::memcpy(short_bytes.data(), ECO_PROF_MAGIC, 8);
    const std::filesystem::path short_path =
        std::filesystem::temp_directory_path() / "echo-profile-short.ecoprof";
    EchoTests::write_file(
        short_path, std::string(reinterpret_cast<char *>(short_bytes.data()), short_bytes.size()));
    REQUIRE_FALSE(Compiler::ProfileFile::open(short_path, file, error));
    REQUIRE(error.find("truncated") != std::string::npos);
    std::filesystem::remove(short_path);
}

TEST_CASE("intern_string reuses an offset, and event_kind peels SITE_BIT", "[profile]")
{
    ProfileFileBuilder b;
    b.intern_string("");
    const uint32_t foo = b.intern_string("foo");
    REQUIRE(b.intern_string("foo") == foo);
    b.set_frames({ 0x10, 0x20, 0x30 });
    b.set_stacks({ { 1, 2 } });
    Compiler::ProfileFile file = std::move(b).build();
    REQUIRE(file.string_at(foo) == "foo");
    REQUIRE(file.string_at(foo + 1000).empty());

    REQUIRE(file.event_kind(ECO_PROF_KIND_ALLOC) == ECO_PROF_KIND_ALLOC);
    REQUIRE(file.event_kind(ECO_PROF_SITE_BIT | 0) == 0);

    const auto frames = file.stack_frames(0);
    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0] == 0x20);
    REQUIRE(file.stack_frames(3).empty());
}

TEST_CASE("a site-tagged event round-trips through the file", "[profile]")
{
    std::vector<unsigned char> bytes = empty_header();

    const char strings[] = "\0Vec3";
    const uint32_t slen = 6;
    std::vector<unsigned char> strs(8 + slen, 0);
    std::memcpy(strs.data(), &slen, 4);
    std::memcpy(strs.data() + 8, strings, slen);
    put_section(bytes, ECO_PROF_TAG_STRS, strs);

    std::vector<unsigned char> site(32, 0);
    uint32_t kind = ECO_PROF_KIND_COPY_CTOR;
    uint64_t type_bytes = 12;
    uint32_t type_name = 1;
    uint64_t function = 0x1000;
    std::memcpy(site.data(), &kind, 4);
    std::memcpy(site.data() + 8, &type_bytes, 8);
    std::memcpy(site.data() + 16, &type_name, 4);
    std::memcpy(site.data() + 24, &function, 8);
    put_section(bytes, ECO_PROF_TAG_SITE, site);

    std::vector<unsigned char> event(24, 0);
    uint32_t site_or_kind = ECO_PROF_SITE_BIT | 0;
    uint32_t stack = 0;
    uint64_t count = 2;
    uint64_t nbytes = 24;
    std::memcpy(event.data(), &site_or_kind, 4);
    std::memcpy(event.data() + 4, &stack, 4);
    std::memcpy(event.data() + 8, &count, 8);
    std::memcpy(event.data() + 16, &nbytes, 8);
    put_section(bytes, ECO_PROF_TAG_EVNT, event);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "echo-profile-sitebit.ecoprof";
    EchoTests::write_file(path, std::string(reinterpret_cast<char *>(bytes.data()), bytes.size()));

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE(Compiler::ProfileFile::open(path, file, error));
    REQUIRE(file.sites().size() == 1);
    REQUIRE(file.sites()[0].kind == ECO_PROF_KIND_COPY_CTOR);
    REQUIRE(file.string_at(file.sites()[0].type_name) == "Vec3");
    REQUIRE(file.events().size() == 1);
    REQUIRE(file.event_kind(file.events()[0].site_or_kind) == ECO_PROF_KIND_COPY_CTOR);
    REQUIRE(file.events()[0].count == 2);
    std::filesystem::remove(path);
}

TEST_CASE("a truncated site section is refused, and an empty file cannot fold", "[profile]")
{
    std::vector<unsigned char> bytes = empty_header();
    std::vector<unsigned char> site(16, 0);
    put_section(bytes, ECO_PROF_TAG_SITE, site);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "echo-profile-bad-site.ecoprof";
    EchoTests::write_file(path, std::string(reinterpret_cast<char *>(bytes.data()), bytes.size()));

    Compiler::ProfileFile file;
    std::string error;
    REQUIRE_FALSE(Compiler::ProfileFile::open(path, file, error));
    REQUIRE(error.find("multiple of 32") != std::string::npos);
    std::filesystem::remove(path);

    Compiler::ProfileFile empty;
    std::ostringstream folded;
    REQUIRE_FALSE(Compiler::ProfileReport::write_folded(
        empty, Compiler::ProfileFoldKind::t_allocs, folded, error));
    REQUIRE(error.find("nothing to fold") != std::string::npos);
}

TEST_CASE("folded calls walks the CCT to collapsed stacks", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.header().flags = ECO_PROF_FLAG_CALLS;
    const uint32_t main_name = b.intern_string("main");
    const uint32_t spawn_name = b.intern_string("spawn");

    Compiler::ProfileSite main_site;
    main_site.kind = ECO_PROF_KIND_CALL;
    main_site.type_name = main_name;
    b.add_site(main_site);

    Compiler::ProfileSite spawn_site;
    spawn_site.kind = ECO_PROF_KIND_CALL;
    spawn_site.type_name = spawn_name;
    b.add_site(spawn_site);

    Compiler::ProfileCct dummy;
    b.add_cct(dummy);

    Compiler::ProfileCct main_node;
    main_node.parent = 0;
    main_node.site = 0;
    main_node.calls = 1;
    b.add_cct(main_node);

    Compiler::ProfileCct spawn_node;
    spawn_node.parent = 1;
    spawn_node.site = 1;
    spawn_node.calls = 4;
    b.add_cct(spawn_node);

    Compiler::ProfileFile file = std::move(b).build();

    std::ostringstream folded;
    std::string error;
    REQUIRE(Compiler::ProfileReport::write_folded(
        file, Compiler::ProfileFoldKind::t_calls, folded, error));
    REQUIRE(folded.str().find("main;spawn 4") != std::string::npos);
    REQUIRE(folded.str().find("main 1") != std::string::npos);
    REQUIRE(folded.str().find("? ") == std::string::npos);

    std::ostringstream summary;
    ProfileReport(summary, TerminalCapabilities::plain()).print_summary(file, "app.ecoprof");
    REQUIRE(summary.str().find("Calls\n") != std::string::npos);
    REQUIRE(summary.str().find("Call paths") != std::string::npos);
    REQUIRE(summary.str().find("spawn") != std::string::npos);
}

TEST_CASE("folded copies combine bytewise and constructor counts", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.intern_string("");
    b.set_frames({ 0x1000 });
    b.set_stacks({ { 0, 1 } });

    Compiler::ProfileEvent copy;
    copy.site_or_kind = ECO_PROF_KIND_COPY_BYTES;
    copy.stack = 0;
    copy.count = 4;
    copy.bytes = 48;
    b.add_event(copy);

    Compiler::ProfileEvent ctor;
    ctor.site_or_kind = ECO_PROF_KIND_COPY_CTOR;
    ctor.stack = 0;
    ctor.count = 1;
    ctor.bytes = 12;
    b.add_event(ctor);
    Compiler::ProfileFile file = std::move(b).build();

    std::ostringstream copies;
    std::string error;
    REQUIRE(Compiler::ProfileReport::write_folded(
        file, Compiler::ProfileFoldKind::t_copies, copies, error));
    REQUIRE(copies.str().find("5") != std::string::npos);

    std::ostringstream copy_bytes;
    REQUIRE(Compiler::ProfileReport::write_folded(
        file, Compiler::ProfileFoldKind::t_copy_bytes, copy_bytes, error));
    REQUIRE(copy_bytes.str().find("60") != std::string::npos);
}

TEST_CASE("echoc profile reads a symbolized file without a binary", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.intern_string("");
    b.set_frames({ 0x1000 });
    b.set_stacks({ { 0, 1 } });
    Compiler::ProfileEvent event;
    event.site_or_kind = ECO_PROF_KIND_ALLOC;
    event.stack = 0;
    event.count = 3;
    event.bytes = 24;
    b.add_event(event);
    b.mark_symbolized();
    Compiler::ProfileFile file = std::move(b).build();

    ScopedProject project("profile", "profile_command");
    const std::filesystem::path path = project.root() / "app.ecoprof";
    std::string error;
    REQUIRE(Compiler::ProfileFile::write(path, file, error));

    const ProcessResult missing = project.echoc("profile");
    REQUIRE(missing.exit_code != 0);
    REQUIRE(missing.output.find(".ecoprof") != std::string::npos);

    const ProcessResult summary = project.echoc("profile app.ecoprof");
    INFO(summary.output);
    REQUIRE(summary.exit_code == 0);
    REQUIRE(summary.output.find("[profile]") != std::string::npos);
    REQUIRE(summary.output.find("3 allocs") != std::string::npos);

    const ProcessResult text = project.echoc("profile --text app.ecoprof");
    REQUIRE(text.exit_code == 0);
    REQUIRE(text.output.find("stacks") != std::string::npos);

    const ProcessResult folded = project.echoc("profile --folded allocs app.ecoprof");
    REQUIRE(folded.exit_code == 0);
    REQUIRE(folded.output.find("0x1000") != std::string::npos);
}

TEST_CASE("a 1M-stack synthetic file reads in one pass", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.header().header_bytes = ECO_PROF_HEADER_BYTES;
    b.intern_string("");
    std::vector<uint64_t> frames(1'000'000, 0x10ull);
    std::vector<Compiler::ProfileStack> stacks(1'000'000);
    for (uint32_t i = 0; i < 1'000'000; i++) {
        stacks[i].first_frame = i;
        stacks[i].depth = 1;
    }
    b.set_frames(std::move(frames));
    b.set_stacks(std::move(stacks));
    Compiler::ProfileFile file = std::move(b).build();

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "echo-profile-1m.ecoprof";
    std::string error;
    REQUIRE(Compiler::ProfileFile::write(path, file, error));

    Compiler::ProfileFile back;
    REQUIRE(Compiler::ProfileFile::open(path, back, error));
    REQUIRE(back.stacks().size() == 1'000'000);
    REQUIRE(back.frames().size() == 1'000'000);
    std::filesystem::remove(path);
}

TEST_CASE("a mismatched binary is refused", "[profile]")
{
    ProfileFileBuilder b;
    b.header().version = ECO_PROF_VERSION;
    b.intern_string("");
    b.binary().path = b.intern_string(EchoTests::echoc_binary());
    std::memset(b.binary().uuid, 0xff, 16);
    Compiler::ProfileFile file = std::move(b).build();

    ScopedProject project("profile", "mismatch");
    const std::filesystem::path path = project.root() / "app.ecoprof";
    std::string error;
    REQUIRE(Compiler::ProfileFile::write(path, file, error));

    const ProcessResult result = project.echoc("profile app.ecoprof");
    INFO(result.output);
    REQUIRE(result.exit_code != 0);
    REQUIRE(result.output.find("does not match") != std::string::npos);
}
