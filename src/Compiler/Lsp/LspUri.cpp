#include "Compiler/Lsp/LspUri.h"

#include "Compiler/SettledPath.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace
{
    int hex_value(char ch)
    {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }

        return -1;
    }

    std::string percent_decode(const std::string &input)
    {
        std::string out;
        out.reserve(input.size());

        for (size_t i = 0; i < input.size(); i++) {
            if (input[i] == '%' && i + 2 < input.size()) {
                const int hi = hex_value(input[i + 1]);
                const int lo = hex_value(input[i + 2]);

                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }

            out.push_back(input[i]);
        }

        return out;
    }

    bool is_unreserved(unsigned char ch)
    {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
    }

    std::string percent_encode_path(const std::string &input)
    {
        std::ostringstream out;
        out << std::hex << std::uppercase;

        for (unsigned char ch : input) {
            if (is_unreserved(ch)) {
                out << static_cast<char>(ch);
            }
            else {
                out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
            }
        }

        return out.str();
    }
};

std::filesystem::path Compiler::Lsp::path_from_uri(const std::string &uri)
{
    std::string rest = uri;

    const std::string scheme = "file:";
    if (rest.rfind(scheme, 0) == 0) {
        rest = rest.substr(scheme.size());
    }

    if (rest.rfind("//", 0) == 0) {
        rest = rest.substr(2);
        const auto slash = rest.find('/');
        if (slash == std::string::npos) {
            return {};
        }

        // authority is empty, `localhost`, or a host we ignore; the path starts at the slash
        rest = rest.substr(slash);
    }

    const std::string decoded = percent_decode(rest);

#ifdef _WIN32
    // file URI with a Windows drive: strip the leading slash so C: is the root
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1]))
        && decoded[2] == ':') {
        return Compiler::canonical_or_absolute(decoded.substr(1));
    }
#endif

    return Compiler::canonical_or_absolute(decoded);
}

std::string Compiler::Lsp::uri_from_path(const std::filesystem::path &path)
{
    const std::filesystem::path absolute = Compiler::canonical_or_absolute(path);
    std::string generic = absolute.generic_string();

#ifdef _WIN32
    if (generic.size() >= 2 && std::isalpha(static_cast<unsigned char>(generic[0])) && generic[1] == ':') {
        generic = "/" + generic;
    }
#endif

    return "file://" + percent_encode_path(generic);
}

bool Compiler::Lsp::is_embedded_stdlib_path(const std::filesystem::path &path)
{
    return path.generic_string().rfind("stdlib:", 0) == 0;
}
