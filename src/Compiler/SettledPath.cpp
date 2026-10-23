#include "Compiler/SettledPath.h"

std::filesystem::path Compiler::canonical_or_absolute(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::canonical(path, ec);

    return ec ? std::filesystem::absolute(path) : resolved;
}

std::filesystem::path Compiler::settled_path(const std::filesystem::path &base, const std::string &spelled)
{
    const std::filesystem::path written(spelled);

    return canonical_or_absolute(written.is_absolute() ? written : base / written);
}

std::filesystem::path Compiler::canonical_or_normal(const std::filesystem::path &path)
{
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);

    if (!ec) {
        return canonical;
    }

    return std::filesystem::absolute(path).lexically_normal();
}

bool Compiler::path_is_inside(const std::filesystem::path &candidate, const std::filesystem::path &bound)
{
    const std::filesystem::path a = canonical_or_normal(candidate);
    const std::filesystem::path b = canonical_or_normal(bound);

    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(a, b, ec);

    if (ec || relative.empty()) {
        return false;
    }

    for (const std::filesystem::path &part : relative) {
        if (part == "..") {
            return false;
        }
    }

    return true;
}
