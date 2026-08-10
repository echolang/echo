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
