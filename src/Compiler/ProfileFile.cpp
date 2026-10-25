#include "Compiler/ProfileFile.h"

#include "Compiler/ArtifactFile.h"
#include "Compiler/ProfileDump.h"

#include <eco_profile.h>

#include <fmt/core.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    uint32_t read_u32(const unsigned char *p)
    {
        uint32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }

    uint64_t read_u64(const unsigned char *p)
    {
        uint64_t v = 0;
        std::memcpy(&v, p, 8);
        return v;
    }

    void write_u32(std::vector<unsigned char> &out, uint32_t v)
    {
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    }

    void write_u64(std::vector<unsigned char> &out, uint64_t v)
    {
        unsigned char b[8];
        std::memcpy(b, &v, 8);
        out.insert(out.end(), b, b + 8);
    }

    void pad4(std::vector<unsigned char> &out)
    {
        while ((out.size() & 3u) != 0) {
            out.push_back(0);
        }
    }

    void write_section(
        std::vector<unsigned char> &out,
        uint32_t tag,
        const unsigned char *payload,
        uint64_t bytes
    )
    {
        write_u32(out, tag);
        write_u32(out, 0);
        write_u64(out, bytes);
        if (bytes != 0 && payload != nullptr) {
            out.insert(out.end(), payload, payload + bytes);
        }
        pad4(out);
    }

    bool parse_events(
        const unsigned char *p,
        uint64_t bytes,
        Compiler::ProfileFileBuilder &out,
        bool live,
        std::string &out_error
    )
    {
        if ((bytes % 24) != 0) {
            out_error = "profile file: an event section is not a multiple of 24 bytes.";
            return false;
        }

        const size_t n = static_cast<size_t>(bytes / 24);
        for (size_t i = 0; i < n; i++) {
            const unsigned char *row = p + i * 24;
            Compiler::ProfileEvent event;
            event.site_or_kind = read_u32(row);
            event.stack = read_u32(row + 4);
            event.count = read_u64(row + 8);
            event.bytes = read_u64(row + 16);
            if (live) {
                out.add_live(event);
            } else {
                out.add_event(event);
            }
        }

        return true;
    }

    bool read_header(
        const unsigned char *data,
        size_t size,
        uint32_t magic_version_cap,
        Compiler::ProfileHeader &header,
        std::string &out_error,
        const char *what
    )
    {
        if (size < 64) {
            out_error = fmt::format("{} is truncated.", what);
            return false;
        }

        header.version = read_u32(data + 8);
        header.header_bytes = read_u32(data + 12);
        header.ticks_per_second = read_u64(data + 16);
        header.status = read_u32(data + 24);
        header.flags = read_u32(data + 28);
        header.anchor_runtime = read_u64(data + 32);
        header.sample_hz = read_u32(data + 40);
        header.thread_count = read_u32(data + 44);
        header.dropped_events = read_u64(data + 48);

        if (header.version > magic_version_cap) {
            out_error = fmt::format(
                "{}: version {} is newer than this echoc.", what, header.version);
            return false;
        }

        if (header.header_bytes < 64 || header.header_bytes > size) {
            out_error = fmt::format("{} is truncated.", what);
            return false;
        }

        return true;
    }

    bool parse_ecoprof(
        const unsigned char *data,
        size_t size,
        Compiler::ProfileFileBuilder &out,
        std::string &out_error
    )
    {
        if (!read_header(
                data, size, ECO_PROF_VERSION, out.header(), out_error, "profile file")) {
            return false;
        }

        Compiler::ProfileHeader &header = out.header();
        size_t cursor = header.header_bytes;

        while (cursor + 16 <= size) {
            const uint32_t tag = read_u32(data + cursor);
            const uint64_t payload = read_u64(data + cursor + 8);
            cursor += 16;

            if (payload > size - cursor) {
                out_error = "profile file: a section is truncated.";
                return false;
            }

            const unsigned char *p = data + cursor;
            const uint64_t padded = (payload + 3u) & ~uint64_t { 3 };
            if (padded > size - cursor) {
                out_error = "profile file: a section is truncated.";
                return false;
            }

            if (tag == ECO_PROF_TAG_STRS) {
                if (payload < 8) {
                    out_error = "profile file: string table is truncated.";
                    return false;
                }

                const uint32_t len = read_u32(p);
                if (len > payload - 8) {
                    out_error = "profile file: string table is truncated.";
                    return false;
                }

                out.set_strings(std::string(reinterpret_cast<const char *>(p + 8), len));
            }
            else if (tag == ECO_PROF_TAG_BINY) {
                if (payload < 24) {
                    out_error = "profile file: binary identity is truncated.";
                    return false;
                }

                out.binary().path = read_u32(p);
                std::memcpy(out.binary().uuid, p + 8, 16);
            }
            else if (tag == ECO_PROF_TAG_SITE) {
                if ((payload % 32) != 0) {
                    out_error = "profile file: a site section is not a multiple of 32 bytes.";
                    return false;
                }

                for (size_t i = 0; i < payload / 32; i++) {
                    const unsigned char *row = p + i * 32;
                    Compiler::ProfileSite site;
                    site.kind = read_u32(row);
                    site.type_bytes = read_u64(row + 8);
                    site.type_name = read_u32(row + 16);
                    site.function_addr = read_u64(row + 24);
                    out.add_site(site);
                }
            }
            else if (tag == ECO_PROF_TAG_FRAM) {
                if ((payload % 8) != 0) {
                    out_error = "profile file: a frame section is not a multiple of 8 bytes.";
                    return false;
                }

                std::vector<uint64_t> frames(static_cast<size_t>(payload / 8));
                for (size_t i = 0; i < frames.size(); i++) {
                    frames[i] = read_u64(p + i * 8);
                }
                out.set_frames(std::move(frames));
            }
            else if (tag == ECO_PROF_TAG_STAK) {
                if ((payload % 8) != 0) {
                    out_error = "profile file: a stack section is not a multiple of 8 bytes.";
                    return false;
                }

                std::vector<Compiler::ProfileStack> stacks(static_cast<size_t>(payload / 8));
                for (size_t i = 0; i < stacks.size(); i++) {
                    stacks[i].first_frame = read_u32(p + i * 8);
                    stacks[i].depth = read_u32(p + i * 8 + 4);
                }
                out.set_stacks(std::move(stacks));
            }
            else if (tag == ECO_PROF_TAG_EVNT) {
                if (!parse_events(p, payload, out, false, out_error)) {
                    return false;
                }
            }
            else if (tag == ECO_PROF_TAG_LIVE) {
                if (!parse_events(p, payload, out, true, out_error)) {
                    return false;
                }
            }
            else if (tag == ECO_PROF_TAG_SMPL) {
                if ((payload % 16) != 0) {
                    out_error = "profile file: a sample section is not a multiple of 16 bytes.";
                    return false;
                }

                for (size_t i = 0; i < payload / 16; i++) {
                    const unsigned char *row = p + i * 16;
                    Compiler::ProfileSample sample;
                    sample.stack = read_u32(row);
                    sample.thread = read_u32(row + 4);
                    sample.samples = read_u64(row + 8);
                    out.add_sample(sample);
                }
            }
            else if (tag == ECO_PROF_TAG_CCTN) {
                if ((payload % 24) != 0) {
                    out_error = "profile file: a call-tree section is not a multiple of 24 bytes.";
                    return false;
                }

                for (size_t i = 0; i < payload / 24; i++) {
                    const unsigned char *row = p + i * 24;
                    Compiler::ProfileCct node;
                    node.parent = read_u32(row);
                    node.site = read_u32(row + 4);
                    node.calls = read_u64(row + 8);
                    node.ticks = read_u64(row + 16);
                    out.add_cct(node);
                }
            }
            else if (tag == ECO_PROF_TAG_ADDR) {
                if ((payload % 8) != 0) {
                    out_error = "profile file: an address section is not a multiple of 8 bytes.";
                    return false;
                }

                std::vector<Compiler::ProfileAddr> addrs(static_cast<size_t>(payload / 8));
                for (size_t i = 0; i < addrs.size(); i++) {
                    addrs[i].first_inline = read_u32(p + i * 8);
                    addrs[i].count = read_u32(p + i * 8 + 4);
                }
                out.set_addrs(std::move(addrs));
            }
            else if (tag == ECO_PROF_TAG_INLN) {
                if ((payload % 16) != 0) {
                    out_error = "profile file: an inline section is not a multiple of 16 bytes.";
                    return false;
                }

                for (size_t i = 0; i < payload / 16; i++) {
                    const unsigned char *row = p + i * 16;
                    Compiler::ProfileInline rec;
                    rec.function = read_u32(row);
                    rec.file = read_u32(row + 4);
                    rec.line = read_u32(row + 8);
                    rec.column = read_u32(row + 12);
                    out.add_inline(rec);
                }
            }

            cursor += static_cast<size_t>(padded);
        }

        return true;
    }
};

bool Compiler::ProfileFile::open(
    const std::filesystem::path &path,
    ProfileFile &out,
    std::string &out_error
)
{
    out = ProfileFile {};
    const std::optional<std::string> bytes = read_whole_file(path);

    if (!bytes.has_value()) {
        out_error = fmt::format("profile file '{}' could not be read.", path.string());
        return false;
    }

    const auto *data = reinterpret_cast<const unsigned char *>(bytes->data());
    const size_t size = bytes->size();

    if (size < 8) {
        out_error = "profile file is truncated.";
        return false;
    }

    if (std::memcmp(data, ECO_DUMP_MAGIC, 8) == 0) {
        ProfileFileBuilder builder;
        if (!parse_profile_dump(data, size, builder, out_error)) {
            return false;
        }
        out = std::move(builder).build();
        return true;
    }

    if (std::memcmp(data, ECO_PROF_MAGIC, 8) != 0) {
        out_error = "profile file: bad magic.";
        return false;
    }

    ProfileFileBuilder builder;
    if (!parse_ecoprof(data, size, builder, out_error)) {
        return false;
    }
    out = std::move(builder).build();
    return true;
}

bool Compiler::ProfileFile::write(
    const std::filesystem::path &path,
    const ProfileFile &file,
    std::string &out_error
)
{
    std::vector<unsigned char> out;
    out.resize(ECO_PROF_HEADER_BYTES, 0);
    std::memcpy(out.data(), ECO_PROF_MAGIC, 8);

    auto put32 = [&](size_t at, uint32_t v) {
        std::memcpy(out.data() + at, &v, 4);
    };
    auto put64 = [&](size_t at, uint64_t v) {
        std::memcpy(out.data() + at, &v, 8);
    };

    put32(8, file._header.version);
    put32(12, ECO_PROF_HEADER_BYTES);
    put64(16, file._header.ticks_per_second);
    put32(24, file._header.status);
    put32(28, file._header.flags);
    put64(32, file._header.anchor_runtime);
    put32(40, file._header.sample_hz);
    put32(44, file._header.thread_count);
    put64(48, file._header.dropped_events);

    {
        const uint32_t len = static_cast<uint32_t>(file._strings.size());
        std::vector<unsigned char> payload(8 + file._strings.size(), 0);
        std::memcpy(payload.data(), &len, 4);
        if (!file._strings.empty()) {
            std::memcpy(payload.data() + 8, file._strings.data(), file._strings.size());
        }
        write_section(out, ECO_PROF_TAG_STRS, payload.data(), payload.size());
    }

    {
        unsigned char payload[24] = {};
        std::memcpy(payload, &file._binary.path, 4);
        std::memcpy(payload + 8, file._binary.uuid, 16);
        write_section(out, ECO_PROF_TAG_BINY, payload, 24);
    }

    if (!file._sites.empty()) {
        std::vector<unsigned char> payload(file._sites.size() * 32, 0);
        for (size_t i = 0; i < file._sites.size(); i++) {
            unsigned char *row = payload.data() + i * 32;
            std::memcpy(row, &file._sites[i].kind, 4);
            std::memcpy(row + 8, &file._sites[i].type_bytes, 8);
            std::memcpy(row + 16, &file._sites[i].type_name, 4);
            std::memcpy(row + 24, &file._sites[i].function_addr, 8);
        }
        write_section(out, ECO_PROF_TAG_SITE, payload.data(), payload.size());
    } else {
        write_section(out, ECO_PROF_TAG_SITE, nullptr, 0);
    }

    if (!file._frames.empty()) {
        write_section(
            out, ECO_PROF_TAG_FRAM,
            reinterpret_cast<const unsigned char *>(file._frames.data()),
            file._frames.size() * 8);
    } else {
        write_section(out, ECO_PROF_TAG_FRAM, nullptr, 0);
    }

    if (!file._stacks.empty()) {
        std::vector<unsigned char> payload(file._stacks.size() * 8);
        for (size_t i = 0; i < file._stacks.size(); i++) {
            std::memcpy(payload.data() + i * 8, &file._stacks[i].first_frame, 4);
            std::memcpy(payload.data() + i * 8 + 4, &file._stacks[i].depth, 4);
        }
        write_section(out, ECO_PROF_TAG_STAK, payload.data(), payload.size());
    } else {
        write_section(out, ECO_PROF_TAG_STAK, nullptr, 0);
    }

    auto write_events = [&](uint32_t tag, const std::vector<ProfileEvent> &events) {
        if (events.empty()) {
            write_section(out, tag, nullptr, 0);
            return;
        }

        std::vector<unsigned char> payload(events.size() * 24, 0);
        for (size_t i = 0; i < events.size(); i++) {
            unsigned char *row = payload.data() + i * 24;
            std::memcpy(row, &events[i].site_or_kind, 4);
            std::memcpy(row + 4, &events[i].stack, 4);
            std::memcpy(row + 8, &events[i].count, 8);
            std::memcpy(row + 16, &events[i].bytes, 8);
        }
        write_section(out, tag, payload.data(), payload.size());
    };

    write_events(ECO_PROF_TAG_EVNT, file._events);
    write_events(ECO_PROF_TAG_LIVE, file._live);

    if (!file._samples.empty()) {
        std::vector<unsigned char> payload(file._samples.size() * 16, 0);
        for (size_t i = 0; i < file._samples.size(); i++) {
            unsigned char *row = payload.data() + i * 16;
            std::memcpy(row, &file._samples[i].stack, 4);
            std::memcpy(row + 4, &file._samples[i].thread, 4);
            std::memcpy(row + 8, &file._samples[i].samples, 8);
        }
        write_section(out, ECO_PROF_TAG_SMPL, payload.data(), payload.size());
    } else {
        write_section(out, ECO_PROF_TAG_SMPL, nullptr, 0);
    }

    if (!file._cct.empty()) {
        std::vector<unsigned char> payload(file._cct.size() * 24, 0);
        for (size_t i = 0; i < file._cct.size(); i++) {
            unsigned char *row = payload.data() + i * 24;
            std::memcpy(row, &file._cct[i].parent, 4);
            std::memcpy(row + 4, &file._cct[i].site, 4);
            std::memcpy(row + 8, &file._cct[i].calls, 8);
            std::memcpy(row + 16, &file._cct[i].ticks, 8);
        }
        write_section(out, ECO_PROF_TAG_CCTN, payload.data(), payload.size());
    }

    if (!file._addrs.empty()) {
        std::vector<unsigned char> payload(file._addrs.size() * 8, 0);
        for (size_t i = 0; i < file._addrs.size(); i++) {
            std::memcpy(payload.data() + i * 8, &file._addrs[i].first_inline, 4);
            std::memcpy(payload.data() + i * 8 + 4, &file._addrs[i].count, 4);
        }
        write_section(out, ECO_PROF_TAG_ADDR, payload.data(), payload.size());
    }

    if (!file._inlines.empty()) {
        std::vector<unsigned char> payload(file._inlines.size() * 16, 0);
        for (size_t i = 0; i < file._inlines.size(); i++) {
            unsigned char *row = payload.data() + i * 16;
            std::memcpy(row, &file._inlines[i].function, 4);
            std::memcpy(row + 4, &file._inlines[i].file, 4);
            std::memcpy(row + 8, &file._inlines[i].line, 4);
            std::memcpy(row + 12, &file._inlines[i].column, 4);
        }
        write_section(out, ECO_PROF_TAG_INLN, payload.data(), payload.size());
    }

    if (!write_artifact_file(path, std::string_view(reinterpret_cast<const char *>(out.data()), out.size()))) {
        out_error = fmt::format("profile file '{}' could not be written.", path.string());
        return false;
    }

    return true;
}

std::string_view Compiler::ProfileFile::string_at(uint32_t offset) const
{
    if (offset >= _strings.size()) {
        return {};
    }

    const char *start = _strings.data() + offset;
    const size_t n = ::strnlen(start, _strings.size() - offset);
    return std::string_view(start, n);
}

Compiler::ProfileFileBuilder::ProfileFileBuilder(ProfileFile file) :
    _file(std::move(file))
{
    rebuild_intern();
}

void Compiler::ProfileFileBuilder::rebuild_intern()
{
    _intern.clear();
    size_t i = 0;
    const std::string &strings = _file._strings;
    while (i < strings.size()) {
        const std::string_view existing = _file.string_at(static_cast<uint32_t>(i));
        _intern.emplace(std::string(existing), static_cast<uint32_t>(i));
        i += existing.size() + 1;
    }
}

void Compiler::ProfileFileBuilder::set_strings(std::string strings)
{
    _file._strings = std::move(strings);
    rebuild_intern();
}

uint32_t Compiler::ProfileFileBuilder::intern_string(std::string_view text)
{
    if (_file._strings.empty()) {
        _file._strings.push_back('\0');
        _intern.emplace(std::string(), 0);
    }

    std::string owned(text);
    const auto it = _intern.find(owned);
    if (it != _intern.end()) {
        return it->second;
    }

    const uint32_t off = static_cast<uint32_t>(_file._strings.size());
    _file._strings.append(owned);
    _file._strings.push_back('\0');
    _intern.emplace(std::move(owned), off);
    return off;
}

void Compiler::ProfileFileBuilder::begin_symbolize()
{
    _file._addrs.assign(_file._frames.size(), ProfileAddr {});
    _file._inlines.clear();
}

void Compiler::ProfileFileBuilder::set_addr(size_t i, uint32_t first, uint32_t count)
{
    _file._addrs[i].first_inline = first;
    _file._addrs[i].count = count;
}

void Compiler::ProfileFileBuilder::mark_symbolized()
{
    _file._header.flags |= ECO_PROF_FLAG_SYMBOLIZED;
}

Compiler::ProfileFile Compiler::ProfileFileBuilder::build() &&
{
    if (_file._header.version == 0) {
        _file._header.version = ECO_PROF_VERSION;
    }
    if (_file._header.header_bytes == 0) {
        _file._header.header_bytes = ECO_PROF_HEADER_BYTES;
    }
    return std::move(_file);
}

uint32_t Compiler::ProfileFile::event_kind(uint32_t site_or_kind) const
{
    if ((site_or_kind & ECO_PROF_SITE_BIT) == 0) {
        return site_or_kind;
    }

    const uint32_t idx = site_or_kind & ~ECO_PROF_SITE_BIT;
    if (idx >= _sites.size()) {
        return 0;
    }

    return _sites[idx].kind;
}

bool Compiler::ProfileFile::symbolized() const
{
    return (_header.flags & ECO_PROF_FLAG_SYMBOLIZED) != 0;
}

std::span<const uint64_t> Compiler::ProfileFile::stack_frames(uint32_t stack) const
{
    if (stack >= _stacks.size()) {
        return {};
    }

    const ProfileStack &rec = _stacks[stack];
    if (rec.first_frame + rec.depth > _frames.size()) {
        return {};
    }

    return std::span<const uint64_t>(_frames.data() + rec.first_frame, rec.depth);
}
