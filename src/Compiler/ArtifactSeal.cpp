#include "Compiler/ArtifactSeal.h"

#include "Compiler/ArtifactFile.h"
#include "Compiler/BuildLayout.h"

#include <llvm-c/blake3.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/RandomNumberGenerator.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{

typedef std::array<uint8_t, LLVM_BLAKE3_KEY_LEN> SealKey;

// a seal of one version of this scheme is never mistaken for one of another
constexpr std::string_view k_seal_domain = "echoc-artifact-seal-v1";

constexpr std::string_view k_key_file = "cache-key";

std::optional<SealKey> key_from_hex(const std::string &text)
{
    std::string hex = text;

    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) {
        hex.pop_back();
    }

    std::string bytes;

    if (!llvm::tryGetFromHex(hex, bytes) || bytes.size() != LLVM_BLAKE3_KEY_LEN) {
        return std::nullopt;
    }

    SealKey key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::optional<SealKey> fresh_key()
{
    SealKey key{};

    if (llvm::getRandomBytes(key.data(), key.size())) {
        return std::nullopt;
    }

    return key;
}

bool key_file_is_ours(const std::filesystem::path &path)
{
#if defined(_WIN32)
    (void)path;
    return true;
#else
    struct stat info {};

    return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid();
#endif
}

// **a key only this user can read, or none.** a key another account could read would let it seal into this user's builds
bool key_file_is_private(const std::filesystem::path &path)
{
#if defined(_WIN32)
    // the per-user profile directory is the boundary on Windows; its ACL is not something to re-derive here
    (void)path;
    return true;
#else
    struct stat info {};

    return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid()
        && (info.st_mode & 077) == 0;
#endif
}

// missing or invalid: write a new key. two repairs at once can disagree (cache misses, not a
// security problem). a create race still loses: the second build reads the first's
std::optional<SealKey> stored_key(const std::filesystem::path &root)
{
    if (root.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path path = root / k_key_file;

    std::error_code ec;

    auto parse_existing = [&]() -> std::optional<SealKey> {
        const std::optional<std::string> text = Compiler::read_whole_file(path);

        if (!text.has_value()) {
            return std::nullopt;
        }

        return key_from_hex(text.value());
    };

    if (std::filesystem::exists(path, ec)) {
        if (std::optional<SealKey> existing = parse_existing()) {
            if (key_file_is_private(path)) {
                return existing;
            }

            if (key_file_is_ours(path)) {
                // a FS that reports 0777 still hashes with this key; deleting it
                // is what races concurrent builds and turns every run into a miss
                llvm::sys::fs::setPermissions(
                    path.string(), llvm::sys::fs::owner_read | llvm::sys::fs::owner_write);
                return existing;
            }

            // foreign owner: do not use, do not delete
            return std::nullopt;
        }

        // truncated or unreadable: recreate only a file we own
        if (!key_file_is_ours(path)) {
            return std::nullopt;
        }

        std::filesystem::remove(path, ec);
    }

    std::filesystem::create_directories(root, ec);

    const std::optional<SealKey> key = fresh_key();

    if (!key.has_value()) {
        return std::nullopt;
    }

    const std::string hex = llvm::toHex(llvm::ArrayRef<uint8_t>(key->data(), key->size()), true) + "\n";

    if (!Compiler::write_artifact_file(
            path, hex, llvm::sys::fs::owner_read | llvm::sys::fs::owner_write)) {
        // lost the create race: the first writer's key is the one to keep
        return parse_existing();
    }

    return parse_existing();
}

// **none at all when the system cannot supply randomness**, and then nothing is sealed and nothing is a hit.
// a fixed fallback key would be one anybody could sign with
const std::optional<SealKey> &process_key()
{
    // a function-local static: read once, thread-safe, and the same answer for the whole process
    static const std::optional<SealKey> key = []() -> std::optional<SealKey> {
        if (const std::optional<SealKey> stored = stored_key(Compiler::user_cache_root())) {
            return stored;
        }

        // nothing earlier runs left behind will verify against this, which is the point
        return fresh_key();
    }();

    return key;
}

std::string memo_key(const std::filesystem::path &artifact, std::string_view context)
{
    std::string key = artifact.string();
    key.push_back('\0');
    key.append(context);
    return key;
}

std::mutex &memo_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, bool> &memo()
{
    static std::map<std::string, bool> answers;
    return answers;
}

};

std::filesystem::path Compiler::seal_path_of(const std::filesystem::path &artifact)
{
    return artifact.string() + ".seal";
}

std::optional<std::string> Compiler::artifact_seal(const std::filesystem::path &artifact, std::string_view context)
{
    const std::optional<SealKey> &key = process_key();

    if (!key.has_value()) {
        return std::nullopt;
    }

    std::ifstream in(artifact, std::ios::binary);

    if (!in) {
        return std::nullopt;
    }

    llvm_blake3_hasher hasher;
    llvm_blake3_hasher_init_keyed(&hasher, key->data());

    // every field closed by a NUL, so a name and a context cannot be re-split into another pair that hashes alike
    const std::string name = artifact.filename().string();
    llvm_blake3_hasher_update(&hasher, k_seal_domain.data(), k_seal_domain.size());
    llvm_blake3_hasher_update(&hasher, "\0", 1);
    llvm_blake3_hasher_update(&hasher, name.data(), name.size());
    llvm_blake3_hasher_update(&hasher, "\0", 1);
    llvm_blake3_hasher_update(&hasher, context.data(), context.size());
    llvm_blake3_hasher_update(&hasher, "\0", 1);

    std::array<char, 1 << 16> buffer{};

    while (in) {
        in.read(buffer.data(), buffer.size());
        llvm_blake3_hasher_update(&hasher, buffer.data(), static_cast<size_t>(in.gcount()));
    }

    if (in.bad()) {
        return std::nullopt;
    }

    std::array<uint8_t, LLVM_BLAKE3_OUT_LEN> out{};
    llvm_blake3_hasher_finalize(&hasher, out.data(), out.size());

    return llvm::toHex(llvm::ArrayRef<uint8_t>(out.data(), out.size()), true);
}

bool Compiler::seal_artifact(const std::filesystem::path &artifact, std::string_view context)
{
    const std::optional<std::string> seal = artifact_seal(artifact, context);
    const bool written = seal.has_value() && write_artifact_file(seal_path_of(artifact), seal.value() + "\n");

    std::lock_guard<std::mutex> lock(memo_mutex());
    memo()[memo_key(artifact, context)] = written;

    return written;
}

bool Compiler::artifact_is_sealed(const std::filesystem::path &artifact, std::string_view context)
{
    const std::string key = memo_key(artifact, context);

    {
        std::lock_guard<std::mutex> lock(memo_mutex());
        const auto found = memo().find(key);

        if (found != memo().end()) {
            return found->second;
        }
    }

    const std::optional<std::string> recorded = read_whole_file(seal_path_of(artifact));
    const std::optional<std::string> actual =
        recorded.has_value() ? artifact_seal(artifact, context) : std::nullopt;
    const bool sealed = actual.has_value() && recorded.value() == actual.value() + "\n";

    std::lock_guard<std::mutex> lock(memo_mutex());
    memo()[key] = sealed;

    return sealed;
}
