#include "Compiler/ArtifactFile.h"

#include "Compiler/BuildLayout.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <optional>

bool Compiler::write_artifact_file(
    const std::filesystem::path &path,
    std::string_view contents,
    std::optional<unsigned> mode
)
{
    const unsigned file_mode =
        mode.value_or(llvm::sys::fs::all_read | llvm::sys::fs::all_write);

    llvm::Expected<llvm::sys::fs::TempFile> staged =
        llvm::sys::fs::TempFile::create(path.string() + ".tmp-%%%%%%%%", file_mode);

    if (!staged) {
        llvm::consumeError(staged.takeError());
        return false;
    }

    bool written = false;

    {
        llvm::raw_fd_ostream out(staged->FD, false);
        out << contents;
        out.flush();

        written = !out.has_error();
        out.clear_error();
    }

    if (!written) {
        llvm::consumeError(staged->discard());
        return false;
    }

    if (llvm::Error kept = staged->keep(path.string())) {
        llvm::consumeError(std::move(kept));
        llvm::consumeError(staged->discard());
        return false;
    }

    return true;
}

void Compiler::clear_artifact_path(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::optional<std::string> Compiler::read_whole_file(const std::filesystem::path &path)
{
    if (is_embedded_stdlib_path(path)) {
        return embedded_stdlib_content(path);
    }

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }

    const std::streamoff size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    std::string bytes(static_cast<size_t>(size), '\0');

    in.seekg(0);
    in.read(bytes.data(), size);

    if (in.bad()) {
        return std::nullopt;
    }

    bytes.resize(static_cast<size_t>(in.gcount()));

    return bytes;
}
