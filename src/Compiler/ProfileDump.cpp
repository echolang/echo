#include "Compiler/ProfileDump.h"

#include <eco_profile.h>

#include <fmt/core.h>

#include <cstring>
#include <string>
#include <string_view>
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

    bool read_header(
        const unsigned char *data,
        size_t size,
        Compiler::ProfileHeader &header,
        std::string &out_error
    )
    {
        if (size < 64) {
            out_error = "profile dump is truncated.";
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

        if (header.version > ECO_DUMP_VERSION) {
            out_error = fmt::format(
                "profile dump: version {} is newer than this echoc.", header.version);
            return false;
        }

        if (header.header_bytes < 64 || header.header_bytes > size) {
            out_error = "profile dump is truncated.";
            return false;
        }

        return true;
    }

    struct DumpCursor
    {
        const unsigned char *p = nullptr;
        const unsigned char *end = nullptr;
        bool ok = true;

        bool need(size_t n)
        {
            if (!ok || p + n > end) {
                ok = false;
                return false;
            }
            return true;
        }

        uint32_t u32()
        {
            if (!need(4)) {
                return 0;
            }
            const uint32_t v = read_u32(p);
            p += 4;
            return v;
        }

        uint64_t u64()
        {
            if (!need(8)) {
                return 0;
            }
            const uint64_t v = read_u64(p);
            p += 8;
            return v;
        }

        void skip(size_t n)
        {
            if (need(n)) {
                p += n;
            }
        }

        void pad4(uint32_t n)
        {
            const uint32_t r = n & 3u;
            if (r != 0) {
                skip(4u - r);
            }
        }

        std::string_view bytes(uint32_t n)
        {
            if (!need(n)) {
                return {};
            }
            const char *start = reinterpret_cast<const char *>(p);
            p += n;
            return std::string_view(start, n);
        }
    };
}

bool Compiler::parse_profile_dump(
    const unsigned char *data,
    size_t size,
    ProfileFileBuilder &out,
    std::string &out_error
)
{
    if (!read_header(data, size, out.header(), out_error)) {
        return false;
    }

    ProfileHeader &header = out.header();
    const uint32_t dump_header_bytes = header.header_bytes;
    header.version = ECO_PROF_VERSION;
    header.header_bytes = ECO_PROF_HEADER_BYTES;

    DumpCursor cur { data + dump_header_bytes, data + size, true };
    out.intern_string("");

    const uint32_t path_len = cur.u32();
    const std::string_view path = cur.bytes(path_len);
    cur.pad4(path_len);
    if (cur.ok && !path.empty()) {
        out.binary().path = out.intern_string(path);
    }

    struct LiveHit
    {
        uint32_t stack = 0;
        uint32_t thread = 0;
        uint64_t size = 0;
    };

    const uint32_t live_n = cur.u32();
    if (live_n != 0 && !cur.need(static_cast<size_t>(live_n) * 16)) {
        out_error = "profile dump: a section is truncated.";
        return false;
    }

    std::vector<LiveHit> live_hits(live_n);
    for (uint32_t i = 0; i < live_n && cur.ok; i++) {
        live_hits[i].stack = cur.u32();
        live_hits[i].thread = cur.u32();
        live_hits[i].size = cur.u64();
    }

    struct ThreadBase
    {
        uint32_t thread_id = 0;
        uint32_t stack_base = 0;
        uint32_t site_base = 0;
        uint32_t stack_count = 0;
    };

    std::vector<ThreadBase> bases;
    std::vector<uint64_t> all_frames;
    std::vector<ProfileStack> all_stacks;
    uint32_t stack_base = 0;
    uint32_t site_base = 0;
    uint32_t frame_base = 0;
    uint32_t cct_base = 0;

    for (uint32_t t = 0; t < header.thread_count && cur.ok; t++) {
        ThreadBase base;
        base.thread_id = cur.u32();
        cur.u32();
        const uint32_t site_count = cur.u32();
        const uint32_t frame_count = cur.u32();
        const uint32_t stack_count = cur.u32();
        const uint32_t event_count = cur.u32();
        const uint32_t sample_count = cur.u32();
        const uint32_t cct_count = cur.u32();

        base.stack_base = stack_base;
        base.site_base = site_base;
        base.stack_count = stack_count;
        bases.push_back(base);

        for (uint32_t i = 0; i < site_count && cur.ok; i++) {
            ProfileSite site;
            site.kind = cur.u32();
            cur.u32();
            site.type_bytes = cur.u64();
            site.function_addr = cur.u64();
            const uint32_t name_len = cur.u32();
            site.type_name = out.intern_string(cur.bytes(name_len));
            cur.pad4(name_len);
            out.add_site(site);
        }

        for (uint32_t i = 0; i < frame_count && cur.ok; i++) {
            all_frames.push_back(cur.u64());
        }

        for (uint32_t i = 0; i < stack_count && cur.ok; i++) {
            ProfileStack rec;
            rec.first_frame = frame_base + cur.u32();
            rec.depth = cur.u32();
            all_stacks.push_back(rec);
        }

        for (uint32_t i = 0; i < event_count && cur.ok; i++) {
            ProfileEvent event;
            uint32_t site = cur.u32();
            uint32_t stack = cur.u32();
            event.count = cur.u64();
            event.bytes = cur.u64();
            if ((site & ECO_PROF_SITE_BIT) != 0) {
                site = ECO_PROF_SITE_BIT | ((site & ~ECO_PROF_SITE_BIT) + site_base);
            }
            event.site_or_kind = site;
            event.stack = stack == 0 ? 0 : (stack - 1) + stack_base;
            out.add_event(event);
        }

        for (uint32_t i = 0; i < sample_count && cur.ok; i++) {
            ProfileSample sample;
            const uint32_t stack = cur.u32();
            sample.thread = cur.u32();
            sample.samples = cur.u64();
            sample.stack = stack == 0 ? 0 : (stack - 1) + stack_base;
            out.add_sample(sample);
        }

        if (cct_count > 1) {
            header.flags |= ECO_PROF_FLAG_CALLS;
        }

        for (uint32_t i = 0; i < cct_count && cur.ok; i++) {
            ProfileCct node;
            uint32_t parent = cur.u32();
            uint32_t site = cur.u32();
            node.calls = cur.u64();
            node.ticks = cur.u64();
            if (i == 0) {
                parent = 0;
            } else if (parent == 0) {
                parent = cct_base;
            } else {
                parent = parent + cct_base;
            }
            if (site != 0) {
                site = (site - 1) + site_base;
            }
            node.parent = parent;
            node.site = site;
            out.add_cct(node);
        }

        stack_base += stack_count;
        site_base += site_count;
        frame_base += frame_count;
        cct_base += cct_count;
    }

    out.set_frames(std::move(all_frames));
    out.set_stacks(std::move(all_stacks));

    for (const LiveHit &hit : live_hits) {
        uint32_t stack = hit.stack;
        for (const ThreadBase &base : bases) {
            if (base.thread_id == hit.thread) {
                stack = stack == 0 ? 0 : (stack - 1) + base.stack_base;
                break;
            }
        }
        ProfileEvent event;
        event.site_or_kind = ECO_PROF_KIND_ALLOC;
        event.stack = stack;
        event.count = 1;
        event.bytes = hit.size;
        out.add_live(event);
    }

    if (!cur.ok) {
        out_error = "profile dump: a section is truncated.";
        return false;
    }

    return true;
}
