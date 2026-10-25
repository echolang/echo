#ifndef PROFILEFILE_H
#define PROFILEFILE_H

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Compiler
{
    struct ProfileHeader
    {
        uint32_t version = 0;
        uint32_t header_bytes = 0;
        uint64_t ticks_per_second = 0;
        uint32_t status = 0;
        uint32_t flags = 0;
        uint64_t anchor_runtime = 0;
        uint32_t sample_hz = 0;
        uint32_t thread_count = 0;
        uint64_t dropped_events = 0;
    };

    struct ProfileSite
    {
        uint32_t kind = 0;
        uint64_t type_bytes = 0;
        uint32_t type_name = 0;
        uint64_t function_addr = 0;
    };

    struct ProfileStack
    {
        uint32_t first_frame = 0;
        uint32_t depth = 0;
    };

    struct ProfileEvent
    {
        uint32_t site_or_kind = 0;
        uint32_t stack = 0;
        uint64_t count = 0;
        uint64_t bytes = 0;
    };

    struct ProfileSample
    {
        uint32_t stack = 0;
        uint32_t thread = 0;
        uint64_t samples = 0;
    };

    struct ProfileCct
    {
        uint32_t parent = 0;
        uint32_t site = 0;
        uint64_t calls = 0;
        uint64_t ticks = 0;
    };

    struct ProfileInline
    {
        uint32_t function = 0;
        uint32_t file = 0;
        uint32_t line = 0;
        uint32_t column = 0;
    };

    struct ProfileAddr
    {
        uint32_t first_inline = 0;
        uint32_t count = 0;
    };

    struct ProfileBinary
    {
        uint32_t path = 0;
        uint8_t uuid[16] = {};
    };

    class ProfileFileBuilder;

    // an .ecoprof. also opens a raw runtime dump and merges it. the dump is not this format
    class ProfileFile
    {
    public:
        static bool open(const std::filesystem::path &path, ProfileFile &out, std::string &out_error);
        static bool write(
            const std::filesystem::path &path,
            const ProfileFile &file,
            std::string &out_error
        );

        const ProfileHeader &header() const { return _header; }
        std::string_view string_at(uint32_t offset) const;
        std::string_view strings() const { return _strings; }

        const ProfileBinary &binary() const { return _binary; }

        const std::vector<ProfileSite> &sites() const { return _sites; }
        const std::vector<uint64_t> &frames() const { return _frames; }
        const std::vector<ProfileStack> &stacks() const { return _stacks; }
        const std::vector<ProfileEvent> &events() const { return _events; }
        const std::vector<ProfileEvent> &live() const { return _live; }
        const std::vector<ProfileSample> &samples() const { return _samples; }
        const std::vector<ProfileCct> &cct() const { return _cct; }
        const std::vector<ProfileAddr> &addrs() const { return _addrs; }
        const std::vector<ProfileInline> &inlines() const { return _inlines; }

        uint32_t event_kind(uint32_t site_or_kind) const;
        bool symbolized() const;
        std::span<const uint64_t> stack_frames(uint32_t stack) const;

    private:
        friend class ProfileFileBuilder;

        ProfileHeader _header;
        std::string _strings;
        ProfileBinary _binary;
        std::vector<ProfileSite> _sites;
        std::vector<uint64_t> _frames;
        std::vector<ProfileStack> _stacks;
        std::vector<ProfileEvent> _events;
        std::vector<ProfileEvent> _live;
        std::vector<ProfileSample> _samples;
        std::vector<ProfileCct> _cct;
        std::vector<ProfileAddr> _addrs;
        std::vector<ProfileInline> _inlines;
    };

    // construction and symbolize. intern is a map, so a large symbolize is not quadratic
    class ProfileFileBuilder
    {
    public:
        ProfileFileBuilder() = default;
        explicit ProfileFileBuilder(ProfileFile file);

        ProfileHeader &header() { return _file._header; }
        ProfileBinary &binary() { return _file._binary; }

        uint32_t intern_string(std::string_view text);
        std::string_view string_at(uint32_t offset) const { return _file.string_at(offset); }
        void set_strings(std::string strings);

        void add_site(ProfileSite site) { _file._sites.push_back(site); }

        void set_frames(std::vector<uint64_t> frames) { _file._frames = std::move(frames); }
        const std::vector<uint64_t> &frames() const { return _file._frames; }

        void set_stacks(std::vector<ProfileStack> stacks) { _file._stacks = std::move(stacks); }

        void add_event(ProfileEvent event) { _file._events.push_back(event); }
        void add_live(ProfileEvent event) { _file._live.push_back(event); }
        void add_sample(ProfileSample sample) { _file._samples.push_back(sample); }
        void add_cct(ProfileCct node) { _file._cct.push_back(node); }

        void set_addrs(std::vector<ProfileAddr> addrs) { _file._addrs = std::move(addrs); }

        void begin_symbolize();
        void add_inline(ProfileInline rec) { _file._inlines.push_back(rec); }
        void set_addr(size_t i, uint32_t first, uint32_t count);
        void mark_symbolized();

        const std::vector<ProfileInline> &inlines() const { return _file._inlines; }

        ProfileFile build() &&;

    private:
        ProfileFile _file;
        std::unordered_map<std::string, uint32_t> _intern;

        void rebuild_intern();
    };
};

#endif
