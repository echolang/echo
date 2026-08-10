#include "Compiler/TerminalCapabilities.h"

#include <fmt/core.h>

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#define ECO_ISATTY(fd) _isatty(fd)
#define ECO_STDOUT_FD 1
#define ECO_STDERR_FD 2
#else
#include <sys/ioctl.h>
#include <unistd.h>
#define ECO_ISATTY(fd) isatty(fd)
#define ECO_STDOUT_FD STDOUT_FILENO
#define ECO_STDERR_FD STDERR_FILENO
#endif

namespace
{
    // an unset variable and an empty one are the same thing to every tool that reads these, so the two
    // collapse here rather than at each of the four call sites
    const char *read_env(const char *name)
    {
        const char *value = std::getenv(name);
        if (value == nullptr || value[0] == '\0') {
            return nullptr;
        }

        return value;
    }

    bool env_contains(const char *name, const char *needle)
    {
        const char *value = read_env(name);
        return value != nullptr && std::strstr(value, needle) != nullptr;
    }

    // **the fd is a parameter and not a constant, and all three readers below take it.** stderr is still
    // the stream this whole file is about; a help page is the one thing asking about the other, and a
    // helper that kept reading the constant would answer about stderr while its caller believed otherwise
    bool stream_is_a_terminal(int fd)
    {
        return ECO_ISATTY(fd) != 0;
    }

    // the two escape hatches every modern CLI honours, in the order they are specified to apply:
    // NO_COLOR wins over CLICOLOR_FORCE, because opting *out* of decoration is the one a user sets
    // globally and a tool must not be able to override
    bool environment_allows_color(int fd)
    {
        if (read_env("NO_COLOR") != nullptr) {
            return false;
        }

        const char *force = read_env("CLICOLOR_FORCE");
        if (force != nullptr && std::strcmp(force, "0") != 0) {
            return true;
        }

        const char *term = read_env("TERM");
        if (term != nullptr && std::strcmp(term, "dumb") == 0) {
            return false;
        }

        return stream_is_a_terminal(fd);
    }

    // **asked of the locale and not of the terminal**, because it is the encoding of the bytes we are
    // about to write that decides whether they render, and that is what the locale describes. Windows
    // Terminal is checked by name for the one case the locale cannot answer: the legacy console the same
    // binary may equally be run from is the reason a `--diagnostics=ascii` escape hatch exists at all
    bool environment_allows_unicode(int fd)
    {
        if (!stream_is_a_terminal(fd)) {
            return false;
        }

        if (read_env("WT_SESSION") != nullptr) {
            return true;
        }

        // the three POSIX locale variables, in the order they override one another, each in both of the
        // spellings a system may use
        for (const char *variable : { "LC_ALL", "LC_CTYPE", "LANG" }) {
            if (env_contains(variable, "UTF-8") || env_contains(variable, "utf8")) {
                return true;
            }
        }

        return false;
    }

    unsigned int terminal_width(int fd)
    {
        if (!stream_is_a_terminal(fd)) {
            return 0;
        }

        const char *columns = read_env("COLUMNS");
        if (columns != nullptr) {
            const int parsed = std::atoi(columns);
            if (parsed > 0) {
                return static_cast<unsigned int>(parsed);
            }
        }

#if !defined(_WIN32)
        struct winsize size;
        if (ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
            return size.ws_col;
        }
#endif

        return 0;
    }
};

Compiler::TerminalCapabilities Compiler::TerminalCapabilities::resolve(
    ColorChoice color_choice,
    DiagnosticFormat format,
    TerminalStream stream
)
{
    TerminalCapabilities caps;

    // resolved once and handed to all four readers below, so a stream added here cannot reach three of
    // them and miss the fourth
    const int fd = stream == TerminalStream::t_stdout ? ECO_STDOUT_FD : ECO_STDERR_FD;

    switch (color_choice) {
    case ColorChoice::t_always:
        caps.color = true;
        break;
    case ColorChoice::t_never:
        caps.color = false;
        break;
    case ColorChoice::t_auto:
        caps.color = environment_allows_color(fd);
        break;
    }

    switch (format) {
    case DiagnosticFormat::t_pretty:
        caps.unicode = true;
        break;
    case DiagnosticFormat::t_ascii:
    // json carries no glyphs at all, and answering `true` here would let a caller that forgot to branch
    // on the format write one into a stream that is parsed rather than read
    case DiagnosticFormat::t_json:
        caps.unicode = false;
        break;
    case DiagnosticFormat::t_auto:
        caps.unicode = environment_allows_unicode(fd);
        break;
    }

    caps.width = terminal_width(fd);

    // no flag consults this one - see the field's comment. It is the same isatty the two `auto` paths
    // above already reached, asked once more rather than a second time somewhere else
    caps.interactive = stream_is_a_terminal(fd);

    return caps;
}

Compiler::TerminalCapabilities Compiler::TerminalCapabilities::plain()
{
    return TerminalCapabilities();
}

namespace
{
    // **the accepted spellings and the message that lists them, out of one table.** the two flag parsers
    // were the same function twice, each naming its values a second time inside its error string - so a
    // value added to one of the enums could be accepted and still not be offered
    template <typename T>
    bool parse_choice(
        const std::string &value,
        const char *flag,
        std::initializer_list<std::pair<const char *, T>> choices,
        T &out_value,
        std::string &out_error)
    {
        std::string expected;

        for (const auto &[name, choice] : choices) {
            if (value == name) {
                out_value = choice;
                return true;
            }

            // only ever completed on the path that needs it: a match returns above, so reaching the end
            // of the table is exactly the case where every name has been appended
            if (!expected.empty()) {
                expected += ", ";
            }

            expected += name;
        }

        out_error = fmt::format(
            "Unknown '{}' value '{}'. Expected one of: {}", flag, value, expected);

        return false;
    }
};

bool Compiler::parse_color_choice(const std::string &value, ColorChoice &out_choice, std::string &out_error)
{
    return parse_choice<ColorChoice>(value, "--color", {
        { "auto", ColorChoice::t_auto },
        { "always", ColorChoice::t_always },
        { "never", ColorChoice::t_never },
    }, out_choice, out_error);
}

bool Compiler::parse_diagnostic_format(
    const std::string &value,
    DiagnosticFormat &out_format,
    std::string &out_error
)
{
    return parse_choice<DiagnosticFormat>(value, "--diagnostics", {
        { "auto", DiagnosticFormat::t_auto },
        { "pretty", DiagnosticFormat::t_pretty },
        { "ascii", DiagnosticFormat::t_ascii },
        { "json", DiagnosticFormat::t_json },
    }, out_format, out_error);
}
