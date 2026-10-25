#include "Compiler/ProfileReport.h"

#include "Compiler/TerminalStyle.h"

#include <eco_profile.h>

#include <fmt/core.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr size_t k_summary_rows = 5;
    constexpr size_t k_text_rows = 10;

    struct FrameName
    {
        std::string function;
        std::string file;
        uint32_t line = 0;
    };

    bool is_runtime_frame(const std::string &function)
    {
        return function.rfind("__eco_", 0) == 0
            || function.rfind("mem::", 0) == 0
            || function.rfind("mem.", 0) == 0;
    }

    std::vector<FrameName> named_stack(const Compiler::ProfileFile &file, uint32_t stack)
    {
        std::vector<FrameName> names;
        const auto frames = file.stack_frames(stack);
        if (frames.empty()) {
            return names;
        }

        if (file.symbolized() && !file.addrs().empty()) {
            for (size_t i = 0; i < frames.size(); i++) {
                const uint32_t index = file.stacks()[stack].first_frame + static_cast<uint32_t>(i);
                if (index >= file.addrs().size()) {
                    continue;
                }

                const Compiler::ProfileAddr &addr = file.addrs()[index];
                if (addr.count == 0 || addr.first_inline + addr.count > file.inlines().size()) {
                    FrameName name;
                    name.function = fmt::format("{:#x}", frames[i]);
                    names.push_back(name);
                    continue;
                }

                for (uint32_t n = 0; n < addr.count; n++) {
                    const Compiler::ProfileInline &in = file.inlines()[addr.first_inline + n];
                    FrameName name;
                    name.function = std::string(file.string_at(in.function));
                    name.file = std::string(file.string_at(in.file));
                    name.line = in.line;
                    if (name.function.empty() || name.function == "<invalid>") {
                        name.function = fmt::format("{:#x}", frames[i]);
                    }
                    names.push_back(name);
                }
            }
        } else {
            for (uint64_t addr : frames) {
                FrameName name;
                name.function = fmt::format("{:#x}", addr);
                names.push_back(name);
            }
        }

        return names;
    }

    std::string collapsed(const std::vector<FrameName> &names, bool fold_runtime)
    {
        // names are leaf-first. skip the runtime thunks at the leaf so a path
        // reads `main;work;push` rather than ending on `__eco_prof_realloc`
        size_t from = 0;
        if (fold_runtime) {
            while (from < names.size() && is_runtime_frame(names[from].function)) {
                from += 1;
            }
        }

        std::string out;
        for (size_t i = names.size(); i-- > from;) {
            const FrameName &name = names[i];
            if (!out.empty()) {
                out += ';';
            }
            out += name.function.empty() ? "?" : name.function;
        }
        return out;
    }

    std::string leaf(const std::vector<FrameName> &names)
    {
        for (const FrameName &name : names) {
            if (!is_runtime_frame(name.function) && !name.function.empty()) {
                return name.function;
            }
        }
        return names.empty() ? "?" : names.front().function;
    }

    std::vector<FrameName> named_cct(const Compiler::ProfileFile &file, uint32_t index)
    {
        std::vector<FrameName> names;
        const std::vector<Compiler::ProfileCct> &cct = file.cct();
        uint32_t steps = 0;
        while (index < cct.size() && steps < cct.size()) {
            const Compiler::ProfileCct &node = cct[index];
            if (node.calls == 0) {
                break;
            }

            FrameName name;
            if (node.site < file.sites().size()) {
                name.function = std::string(file.string_at(file.sites()[node.site].type_name));
            }
            if (name.function.empty()) {
                name.function = "?";
            }
            names.push_back(name);

            if (node.parent == index) {
                break;
            }
            index = node.parent;
            steps += 1;
        }
        return names;
    }

    struct CctRow
    {
        std::string path;
        std::string function;
        uint64_t calls = 0;
    };

    std::vector<CctRow> cct_rows(const Compiler::ProfileFile &file)
    {
        std::vector<CctRow> rows;
        const std::vector<Compiler::ProfileCct> &cct = file.cct();
        for (uint32_t i = 0; i < static_cast<uint32_t>(cct.size()); i++) {
            if (cct[i].calls == 0) {
                continue;
            }
            const std::vector<FrameName> names = named_cct(file, i);
            CctRow row;
            row.path = collapsed(names, true);
            row.function = leaf(names);
            row.calls = cct[i].calls;
            rows.push_back(std::move(row));
        }
        return rows;
    }

    struct CountRow
    {
        std::string name;
        uint64_t count = 0;
        uint64_t bytes = 0;
    };

    struct ProfileAnalysis
    {
        uint64_t allocs = 0;
        uint64_t alloc_bytes = 0;
        uint64_t reallocs = 0;
        uint64_t realloc_bytes = 0;
        uint64_t frees = 0;
        uint64_t copies = 0;
        uint64_t copy_bytes = 0;
        uint64_t copy_ctors = 0;
        uint64_t samples = 0;
        uint64_t live_count = 0;
        uint64_t live_bytes = 0;
        uint64_t calls = 0;
        double ms_per_sample = 0.0;
        bool has_calls = false;

        std::map<std::string, CountRow> alloc_paths;
        std::map<std::string, CountRow> live_paths;
        std::map<std::string, CountRow> copy_paths;
        std::map<std::string, uint64_t> self_time;
        std::map<std::string, uint64_t> incl_time;
        std::map<std::string, uint64_t> path_time;
        std::map<std::string, CountRow> call_paths;
        std::map<std::string, CountRow> call_fns;
    };

    ProfileAnalysis analyze(const Compiler::ProfileFile &file)
    {
        ProfileAnalysis a;
        a.ms_per_sample = file.header().sample_hz == 0
            ? 0.0
            : 1000.0 / static_cast<double>(file.header().sample_hz);
        a.has_calls = (file.header().flags & ECO_PROF_FLAG_CALLS) != 0;

        for (const Compiler::ProfileEvent &event : file.events()) {
            const uint32_t kind = file.event_kind(event.site_or_kind);
            const auto names = named_stack(file, event.stack);
            const std::string path = collapsed(names, true);

            if (kind == ECO_PROF_KIND_ALLOC) {
                a.allocs += event.count;
                a.alloc_bytes += event.bytes;
                CountRow &row = a.alloc_paths[path];
                row.name = path;
                row.count += event.count;
                row.bytes += event.bytes;
            } else if (kind == ECO_PROF_KIND_REALLOC) {
                a.reallocs += event.count;
                a.realloc_bytes += event.bytes;
                CountRow &row = a.alloc_paths[path];
                row.name = path;
                row.count += event.count;
                row.bytes += event.bytes;
            } else if (kind == ECO_PROF_KIND_FREE) {
                a.frees += event.count;
            } else if (kind == ECO_PROF_KIND_COPY_BYTES || kind == ECO_PROF_KIND_COPY_CTOR) {
                a.copies += event.count;
                a.copy_bytes += event.bytes;
                if (kind == ECO_PROF_KIND_COPY_CTOR) {
                    a.copy_ctors += event.count;
                }
                CountRow &row = a.copy_paths[path];
                row.name = path;
                row.count += event.count;
                row.bytes += event.bytes;
            }
        }

        for (const Compiler::ProfileEvent &event : file.live()) {
            a.live_count += event.count;
            a.live_bytes += event.bytes;
            const auto names = named_stack(file, event.stack);
            const std::string path = collapsed(names, true);
            CountRow &row = a.live_paths[path];
            row.name = path;
            row.count += event.count;
            row.bytes += event.bytes;
        }

        for (const Compiler::ProfileSample &sample : file.samples()) {
            a.samples += sample.samples;
            const auto names = named_stack(file, sample.stack);
            if (!names.empty()) {
                a.self_time[leaf(names)] += sample.samples;
            }
            std::vector<std::string> seen;
            for (const FrameName &name : names) {
                if (is_runtime_frame(name.function)) {
                    continue;
                }
                if (std::find(seen.begin(), seen.end(), name.function) != seen.end()) {
                    continue;
                }
                seen.push_back(name.function);
                a.incl_time[name.function] += sample.samples;
            }
            a.path_time[collapsed(names, true)] += sample.samples;
        }

        if (a.has_calls) {
            for (const CctRow &row : cct_rows(file)) {
                a.calls += row.calls;
                CountRow &path = a.call_paths[row.path];
                path.name = row.path;
                path.count += row.calls;
                CountRow &fn = a.call_fns[row.function];
                fn.name = row.function;
                fn.count += row.calls;
            }
        }

        return a;
    }

    std::string format_bytes(uint64_t n)
    {
        if (n < 1024) {
            return fmt::format("{} B", n);
        }

        const double kib = static_cast<double>(n) / 1024.0;
        if (n < 1024ull * 1024ull) {
            return fmt::format("{:.1f} KiB", kib);
        }

        const double mib = kib / 1024.0;
        if (n < 1024ull * 1024ull * 1024ull) {
            return fmt::format("{:.1f} MiB", mib);
        }

        return fmt::format("{:.1f} GiB", mib / 1024.0);
    }

    std::string format_ms(double ms)
    {
        if (ms < 1000.0) {
            return fmt::format("{:.1f} ms", ms);
        }

        return fmt::format("{:.2f} s", ms / 1000.0);
    }

    std::string counted(uint64_t n, const char *one, const char *many)
    {
        return fmt::format("{} {}", n, n == 1 ? one : many);
    }

    std::string display_path(std::string_view collapsed, const char *arrow)
    {
        std::string out;
        for (char c : collapsed) {
            if (c == ';') {
                out += arrow;
            } else {
                out += c;
            }
        }
        return out.empty() ? "?" : out;
    }

    const char *status_word(uint32_t status)
    {
        switch (status) {
        case ECO_PROF_STATUS_EXIT:
            return "exited";
        case ECO_PROF_STATUS_ABORT:
            return "aborted";
        default:
            return nullptr;
        }
    }

    enum class TopMode
    {
        t_time,
        t_count,
        t_count_bytes,
        t_bytes
    };

    struct Render
    {
        std::ostream &out;
        const Compiler::ProfileReportTheme &theme;
        bool color;

        std::string styled(const std::string &text, const char *sgr) const {
            return Compiler::styled(text, sgr, color);
        }
    };

    std::vector<CountRow> rows_from_map(const std::map<std::string, CountRow> &map)
    {
        std::vector<CountRow> rows;
        for (const auto &[_, row] : map) {
            rows.push_back(row);
        }
        return rows;
    }

    std::vector<CountRow> rows_from_counts(const std::map<std::string, uint64_t> &map)
    {
        std::vector<CountRow> rows;
        for (const auto &[name, count] : map) {
            rows.push_back({ name, count, 0 });
        }
        return rows;
    }

    void write_top(
        const Render &r,
        const char *title,
        std::vector<CountRow> rows,
        size_t n,
        TopMode mode,
        double ms_per_sample
    )
    {
        const bool by_bytes = mode == TopMode::t_bytes;
        std::sort(rows.begin(), rows.end(), [by_bytes](const CountRow &a, const CountRow &b) {
            if (by_bytes) {
                if (a.bytes != b.bytes) {
                    return a.bytes > b.bytes;
                }
                return a.count > b.count;
            }
            if (a.count != b.count) {
                return a.count > b.count;
            }
            return a.bytes > b.bytes;
        });

        r.out << '\n' << r.styled(title, Compiler::sgr::bold) << '\n';
        const size_t show = std::min(n, rows.size());
        for (size_t i = 0; i < show; i++) {
            const CountRow &row = rows[i];
            std::string first;
            std::string second;
            if (mode == TopMode::t_time) {
                first = fmt::format("{:>10}", format_ms(static_cast<double>(row.count) * ms_per_sample));
            } else {
                first = fmt::format("{:>8}", row.count);
                if (mode == TopMode::t_count_bytes || mode == TopMode::t_bytes) {
                    second = fmt::format("{:>10}", format_bytes(row.bytes));
                }
            }

            r.out << "  " << r.styled(first, Compiler::sgr::secondary);
            if (!second.empty()) {
                r.out << "  " << r.styled(second, Compiler::sgr::secondary);
            }
            r.out << "  " << display_path(row.name, r.theme.arrow) << '\n';
        }
        if (rows.empty()) {
            r.out << "  (none)\n";
        }
    }

    void write_heading(const Render &r, const std::filesystem::path &path)
    {
        if (r.theme.chips) {
            r.out << "  " << r.styled(" PROFILE ", Compiler::sgr::info_badge);
            if (!path.empty()) {
                r.out << "  " << path.string();
            }
            r.out << '\n';
            return;
        }

        r.out << "[profile]";
        if (!path.empty()) {
            r.out << "  " << path.string();
        }
        r.out << '\n';
    }

    void write_headline(const Render &r, const Compiler::ProfileFile &file, const ProfileAnalysis &a)
    {
        r.out << '\n';

        if (a.samples != 0) {
            r.out << "  "
                  << r.styled(
                         format_ms(static_cast<double>(a.samples) * a.ms_per_sample),
                         Compiler::sgr::secondary)
                  << " sampled at " << file.header().sample_hz << " Hz\n";
        }

        r.out << "  " << counted(a.allocs, "alloc", "allocs")
              << " (" << format_bytes(a.alloc_bytes) << ")"
              << r.theme.dot << counted(a.reallocs, "realloc", "reallocs")
              << " (" << format_bytes(a.realloc_bytes) << ")"
              << r.theme.dot << counted(a.frees, "free", "frees") << '\n';

        r.out << "  " << counted(a.live_count, "live at exit", "live at exit")
              << " (" << format_bytes(a.live_bytes) << ")\n";

        if (a.copies != 0) {
            r.out << "  " << counted(a.copies, "copy", "copies")
                  << " (" << format_bytes(a.copy_bytes) << "): "
                  << counted(a.copies - a.copy_ctors, "bytewise", "bytewise")
                  << ", "
                  << counted(a.copy_ctors, "constructor", "constructors") << '\n';
        }

        if (a.has_calls && a.calls != 0) {
            r.out << "  " << counted(a.calls, "call", "calls") << '\n';
        }

        r.out << "  " << counted(file.header().thread_count, "thread", "threads") << '\n';

        if (file.header().dropped_events != 0) {
            r.out << "  "
                  << r.styled(
                         counted(file.header().dropped_events, "event dropped", "events dropped"),
                         Compiler::sgr::warning)
                  << '\n';
        }

        if (file.header().status != ECO_PROF_STATUS_OK) {
            const char *word = status_word(file.header().status);
            const std::string line = word == nullptr
                ? fmt::format("status {}", file.header().status)
                : std::string(word);
            r.out << "  " << r.styled(line, Compiler::sgr::warning) << '\n';
        }
    }
};

Compiler::ProfileReportTheme Compiler::ProfileReportTheme::pretty()
{
    return ProfileReportTheme { " → ", "  ·  ", true };
}

Compiler::ProfileReportTheme Compiler::ProfileReportTheme::ascii()
{
    return ProfileReportTheme { " -> ", "  |  ", false };
}

Compiler::ProfileReport::ProfileReport(std::ostream &out, TerminalCapabilities capabilities) :
    _out(out),
    _capabilities(capabilities),
    _theme(capabilities.unicode ? ProfileReportTheme::pretty() : ProfileReportTheme::ascii())
{}

void Compiler::ProfileReport::print_summary(const ProfileFile &file, const std::filesystem::path &path)
{
    write_report(file, path, k_summary_rows, false);
}

void Compiler::ProfileReport::print_text(const ProfileFile &file, const std::filesystem::path &path)
{
    write_report(file, path, k_text_rows, true);
}

void Compiler::ProfileReport::write_report(
    const ProfileFile &file,
    const std::filesystem::path &path,
    size_t rows,
    bool extra
)
{
    const Render r { _out, _theme, _capabilities.color };
    const ProfileAnalysis a = analyze(file);

    write_heading(r, path);
    write_headline(r, file, a);

    if (!a.self_time.empty()) {
        write_top(r, "Hottest (self)", rows_from_counts(a.self_time), rows, TopMode::t_time, a.ms_per_sample);
    }

    if (!a.incl_time.empty()) {
        write_top(r, "Hottest (inclusive)", rows_from_counts(a.incl_time), rows, TopMode::t_time, a.ms_per_sample);
    }

    if (!a.path_time.empty()) {
        write_top(r, "Hottest paths", rows_from_counts(a.path_time), rows, TopMode::t_time, a.ms_per_sample);
    }

    if (!a.alloc_paths.empty()) {
        const std::vector<CountRow> alloc_rows = rows_from_map(a.alloc_paths);
        write_top(r, "Allocations (count)", alloc_rows, rows, TopMode::t_count_bytes, 0.0);
        write_top(r, "Allocations (bytes)", alloc_rows, rows, TopMode::t_bytes, 0.0);
    }

    if (!a.live_paths.empty()) {
        write_top(r, "Live at exit", rows_from_map(a.live_paths), rows, TopMode::t_bytes, 0.0);
    }

    if (!a.copy_paths.empty()) {
        write_top(r, "Copies", rows_from_map(a.copy_paths), rows, TopMode::t_count_bytes, 0.0);
    }

    if (a.has_calls) {
        if (!a.call_fns.empty()) {
            write_top(r, "Calls", rows_from_map(a.call_fns), rows, TopMode::t_count, 0.0);
        }
        if (!a.call_paths.empty()) {
            write_top(r, "Call paths", rows_from_map(a.call_paths), rows, TopMode::t_count, 0.0);
        }
    }

    if (extra) {
        _out << '\n'
             << "  " << file.stacks().size() << " stacks"
             << _theme.dot << file.frames().size() << " frames"
             << _theme.dot << file.events().size() << " events"
             << _theme.dot << file.samples().size() << " samples\n";
    }
}

bool Compiler::ProfileReport::write_folded(
    const ProfileFile &file,
    ProfileFoldKind kind,
    std::ostream &out,
    std::string &out_error
)
{
    std::map<std::string, uint64_t> rows;

    auto add_event_kind = [&](uint32_t want_kind, bool by_bytes) {
        for (const ProfileEvent &event : file.events()) {
            const uint32_t kind_now = file.event_kind(event.site_or_kind);
            if (kind_now != want_kind
                && !(want_kind == ECO_PROF_KIND_ALLOC
                    && (kind_now == ECO_PROF_KIND_ALLOC || kind_now == ECO_PROF_KIND_REALLOC))) {
                continue;
            }
            const std::string path = collapsed(named_stack(file, event.stack), true);
            rows[path] += by_bytes ? event.bytes : event.count;
        }
    };

    switch (kind) {
    case ProfileFoldKind::t_time:
        for (const ProfileSample &sample : file.samples()) {
            rows[collapsed(named_stack(file, sample.stack), true)] += sample.samples;
        }
        break;
    case ProfileFoldKind::t_self:
        for (const ProfileSample &sample : file.samples()) {
            rows[leaf(named_stack(file, sample.stack))] += sample.samples;
        }
        break;
    case ProfileFoldKind::t_allocs:
        add_event_kind(ECO_PROF_KIND_ALLOC, false);
        break;
    case ProfileFoldKind::t_alloc_bytes:
        add_event_kind(ECO_PROF_KIND_ALLOC, true);
        break;
    case ProfileFoldKind::t_live_bytes:
        for (const ProfileEvent &event : file.live()) {
            rows[collapsed(named_stack(file, event.stack), true)] += event.bytes;
        }
        break;
    case ProfileFoldKind::t_copies:
        add_event_kind(ECO_PROF_KIND_COPY_BYTES, false);
        add_event_kind(ECO_PROF_KIND_COPY_CTOR, false);
        break;
    case ProfileFoldKind::t_copy_bytes:
        add_event_kind(ECO_PROF_KIND_COPY_BYTES, true);
        add_event_kind(ECO_PROF_KIND_COPY_CTOR, true);
        break;
    case ProfileFoldKind::t_calls:
        for (const CctRow &row : cct_rows(file)) {
            rows[row.path] += row.calls;
        }
        break;
    }

    if (rows.empty()) {
        out_error = "nothing to fold.";
        return false;
    }

    for (const auto &[path, count] : rows) {
        if (path.empty()) {
            continue;
        }
        out << path << ' ' << count << '\n';
    }

    return true;
}
