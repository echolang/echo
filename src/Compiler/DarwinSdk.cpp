#include "Compiler/HostTool.h"

#include "Compiler/CodegenTarget.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    std::string trim_right(const std::string &text)
    {
        const size_t end = text.find_last_not_of(" \t\n\r");
        if (end == std::string::npos) {
            return {};
        }

        return text.substr(0, end + 1);
    }
};

std::filesystem::path Compiler::darwin_sdk_root()
{
#if !defined(__APPLE__)
    return {};
#else
    static const std::filesystem::path root = [] {
        if (const char *from_env = std::getenv("SDKROOT"); from_env != nullptr && *from_env != '\0') {
            return std::filesystem::path(from_env);
        }

        // argv, not a shell: HostTool's contract, and the path can contain a space
        const CapturedProcess shown = run_captured({ "xcrun", "--show-sdk-path" });
        if (shown.exit_code != 0) {
            return std::filesystem::path();
        }

        const std::string path = trim_right(shown.output);
        if (path.empty()) {
            return std::filesystem::path();
        }

        return std::filesystem::path(path);
    }();

    return root;
#endif
}

std::string Compiler::darwin_sdk_version()
{
#if !defined(__APPLE__)
    return {};
#else
    static const std::string version = [] {
        const std::filesystem::path sdk_root = darwin_sdk_root();
        if (sdk_root.empty()) {
            return std::string();
        }

        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
            llvm::MemoryBuffer::getFile((sdk_root / "SDKSettings.json").string());
        if (!buffer) {
            return std::string();
        }

        llvm::Expected<llvm::json::Value> parsed = llvm::json::parse((*buffer)->getBuffer());
        if (!parsed) {
            llvm::consumeError(parsed.takeError());
            return std::string();
        }

        const llvm::json::Object *settings = parsed->getAsObject();
        if (settings == nullptr) {
            return std::string();
        }

        const std::optional<llvm::StringRef> field = settings->getString("Version");
        if (!field.has_value() || field->empty()) {
            return std::string();
        }

        return field->str();
    }();

    return version;
#endif
}

std::filesystem::path Compiler::apple_sdk_root(const std::string &sdk)
{
#if !defined(__APPLE__)
    (void)sdk;
    return {};
#else
    if (sdk.empty()) {
        return darwin_sdk_root();
    }

    static std::map<std::string, std::filesystem::path> cache;
    const auto found = cache.find(sdk);
    if (found != cache.end()) {
        return found->second;
    }

    const CapturedProcess shown = run_captured({ "xcrun", "--sdk", sdk, "--show-sdk-path" });
    std::filesystem::path root;
    if (shown.exit_code == 0) {
        const std::string path = trim_right(shown.output);
        if (!path.empty()) {
            root = std::filesystem::path(path);
        }
    }

    cache.emplace(sdk, root);
    return root;
#endif
}

void Compiler::append_darwin_sdk_args(std::vector<std::string> &argv)
{
#if !defined(__APPLE__)
    (void)argv;
#else
    const std::filesystem::path sdk = darwin_sdk_root();
    if (sdk.empty()) {
        return;
    }

    argv.push_back("-isysroot");
    argv.push_back(sdk.string());
#endif
}

bool Compiler::append_apple_target_args(
    std::vector<std::string> &argv,
    const CodegenTarget &target,
    std::string &out_error
)
{
#if !defined(__APPLE__)
    (void)target;
    (void)out_error;
    append_darwin_sdk_args(argv);
    return true;
#else
    if (target.apple_sdk.empty()) {
        append_darwin_sdk_args(argv);
        return true;
    }

    const std::filesystem::path sdk = apple_sdk_root(target.apple_sdk);
    if (sdk.empty()) {
        out_error = "the '" + target.apple_sdk + "' SDK was not found. install Xcode's iOS platform support";
        return false;
    }

    argv.push_back("-isysroot");
    argv.push_back(sdk.string());
    argv.push_back("-target");
    argv.push_back(target.effective_triple());
    return true;
#endif
}
