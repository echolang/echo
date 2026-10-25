#include "Compiler/ProfileSymbolizer.h"

#include <eco_profile.h>

#include <fmt/core.h>

#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Object/BuildID.h>
#include <llvm/Object/MachO.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/Support/Error.h>

#include <utility>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    bool uuid_from_object(llvm::object::ObjectFile &object, std::array<uint8_t, 16> &uuid)
    {
        uuid = {};

        if (auto *macho = llvm::dyn_cast<llvm::object::MachOObjectFile>(&object)) {
            const llvm::ArrayRef<uint8_t> id = macho->getUuid();
            if (!id.empty()) {
                const size_t n = std::min<size_t>(id.size(), 16);
                std::memcpy(uuid.data(), id.data(), n);
                return true;
            }
        }

        const llvm::object::BuildIDRef build_id = llvm::object::getBuildID(&object);
        if (!build_id.empty()) {
            const size_t n = std::min<size_t>(build_id.size(), 16);
            std::memcpy(uuid.data(), build_id.data(), n);
            return true;
        }

        return false;
    }

    uint64_t symbol_address(const llvm::object::SymbolRef &sym)
    {
        llvm::Expected<uint64_t> addr = sym.getAddress();
        if (addr && *addr != 0) {
            return *addr;
        }
        if (!addr) {
            llvm::consumeError(addr.takeError());
        }

        llvm::Expected<uint64_t> value = sym.getValue();
        if (value && *value != 0) {
            return *value;
        }
        if (!value) {
            llvm::consumeError(value.takeError());
        }
        return 0;
    }

    bool anchor_from_object(llvm::object::ObjectFile &object, uint64_t &out_addr)
    {
        // Mach-O writes a STAB with address 0 first, then the real N_SECT. taking the
        // first match made slide 0 and every file address miss both DWARF and the map
        for (const auto &sym : object.symbols()) {
            llvm::Expected<llvm::StringRef> name = sym.getName();
            if (!name) {
                llvm::consumeError(name.takeError());
                continue;
            }

            if (!name->ends_with("eco_prof_anchor")) {
                continue;
            }

            const uint64_t addr = symbol_address(sym);
            if (addr == 0) {
                continue;
            }

            out_addr = addr;
            return true;
        }

        return false;
    }

    std::filesystem::path dsym_dwarf_of(const std::filesystem::path &binary)
    {
        std::filesystem::path dsym = binary;
        dsym += ".dSYM";
        return dsym / "Contents" / "Resources" / "DWARF" / binary.filename();
    }

    bool named(const llvm::DILineInfo &frame)
    {
        return !frame.FunctionName.empty()
            && frame.FunctionName != llvm::DILineInfo::BadString
            && frame.FunctionName != llvm::DILineInfo::Addr2LineBadString;
    }

    // Mach-O puts one extra `_` on every linker name. DWARF `DW_AT_name` is already the Echo
    // spelling, so only a symbol-table name is stripped, or `__eco_*` would miss the runtime fold
    std::string strip_macho_underscore(std::string name)
    {
        if (name.size() > 1 && name.front() == '_') {
            name.erase(0, 1);
        }
        return name;
    }

    struct CoveringSymbol
    {
        uint64_t address = 0;
        uint64_t size = 0;
        std::string name;
    };

    std::vector<CoveringSymbol> covering_symbols(llvm::object::ObjectFile &object)
    {
        std::vector<CoveringSymbol> out;

        for (const auto &pair : llvm::object::computeSymbolSizes(object)) {
            const llvm::object::SymbolRef &sym = pair.first;

            llvm::Expected<uint32_t> flags = sym.getFlags();
            if (!flags) {
                llvm::consumeError(flags.takeError());
                continue;
            }
            if ((*flags & llvm::object::SymbolRef::SF_Undefined) != 0) {
                continue;
            }

            llvm::Expected<llvm::object::SymbolRef::Type> type = sym.getType();
            if (!type) {
                llvm::consumeError(type.takeError());
                continue;
            }
            if (*type != llvm::object::SymbolRef::ST_Function
                && *type != llvm::object::SymbolRef::ST_Unknown) {
                continue;
            }

            llvm::Expected<uint64_t> addr = sym.getAddress();
            if (!addr || *addr == 0) {
                if (!addr) {
                    llvm::consumeError(addr.takeError());
                }
                continue;
            }

            llvm::Expected<llvm::StringRef> name = sym.getName();
            if (!name || name->empty()) {
                if (!name) {
                    llvm::consumeError(name.takeError());
                }
                continue;
            }

            CoveringSymbol rec;
            rec.address = *addr;
            rec.size = pair.second;
            rec.name = name->str();
            out.push_back(std::move(rec));
        }

        std::sort(out.begin(), out.end(), [](const CoveringSymbol &a, const CoveringSymbol &b) {
            if (a.address != b.address) {
                return a.address < b.address;
            }
            return a.size < b.size;
        });
        return out;
    }

    const CoveringSymbol *covering_at(const std::vector<CoveringSymbol> &syms, uint64_t addr)
    {
        if (syms.empty()) {
            return nullptr;
        }

        auto it = std::upper_bound(
            syms.begin(),
            syms.end(),
            addr,
            [](uint64_t value, const CoveringSymbol &sym) {
                return value < sym.address;
            });
        if (it == syms.begin()) {
            return nullptr;
        }
        --it;

        uint64_t end = it->address + it->size;
        if (it->size == 0) {
            auto next = std::next(it);
            end = next == syms.end() ? UINT64_MAX : next->address;
        }
        if (addr >= it->address && addr < end) {
            return &*it;
        }
        return nullptr;
    }

    void consume_error(llvm::Error error)
    {
        llvm::consumeError(std::move(error));
    }

    void identity_from_object(
        llvm::object::ObjectFile &object,
        const std::filesystem::path &binary,
        Compiler::ProfileBinaryIdentity &out
    )
    {
        out = Compiler::ProfileBinaryIdentity {};
        out.path = binary;
        out.has_uuid = uuid_from_object(object, out.uuid);
        out.has_anchor = anchor_from_object(object, out.anchor_file);
    }
}

bool Compiler::ProfileSymbolizer::read_identity(
    const std::filesystem::path &binary,
    ProfileBinaryIdentity &out,
    std::string &out_error
)
{
    auto object = llvm::object::ObjectFile::createObjectFile(binary.string());
    if (!object) {
        out_error = fmt::format("could not open '{}' to symbolize.", binary.string());
        llvm::consumeError(object.takeError());
        return false;
    }

    identity_from_object(*object.get().getBinary(), binary, out);
    return true;
}

bool Compiler::ProfileSymbolizer::symbolize(
    ProfileFile &file,
    const std::filesystem::path &binary,
    std::string &out_error
)
{
    auto exe = llvm::object::ObjectFile::createObjectFile(binary.string());
    if (!exe) {
        out_error = fmt::format("could not open '{}' to symbolize.", binary.string());
        llvm::consumeError(exe.takeError());
        return false;
    }

    llvm::object::OwningBinary<llvm::object::ObjectFile> exe_owning = std::move(exe.get());
    llvm::object::ObjectFile *exe_obj = exe_owning.getBinary();

    ProfileBinaryIdentity identity;
    identity_from_object(*exe_obj, binary, identity);

    ProfileFileBuilder b(std::move(file));

    if (identity.has_uuid) {
        std::memcpy(b.binary().uuid, identity.uuid.data(), 16);
    }
    b.binary().path = b.intern_string(binary.string());

    const uint64_t slide = identity.has_anchor && identity.anchor_file != 0
        ? b.header().anchor_runtime - identity.anchor_file
        : 0;

    llvm::object::OwningBinary<llvm::object::ObjectFile> dwarf_owning;
    llvm::object::ObjectFile *debug_obj = exe_obj;

    const std::filesystem::path dwarf_path = dsym_dwarf_of(binary);
    if (std::filesystem::exists(dwarf_path)) {
        auto dwarf_file = llvm::object::ObjectFile::createObjectFile(dwarf_path.string());
        if (dwarf_file) {
            dwarf_owning = std::move(dwarf_file.get());
            debug_obj = dwarf_owning.getBinary();
        } else {
            llvm::consumeError(dwarf_file.takeError());
        }
    }

    std::unique_ptr<llvm::DWARFContext> dwarf = llvm::DWARFContext::create(
        *debug_obj,
        llvm::DWARFContext::ProcessDebugRelocations::Process,
        nullptr,
        "",
        consume_error,
        consume_error
    );
    if (dwarf == nullptr) {
        out_error = fmt::format("could not read debug info from '{}'.", binary.string());
        file = std::move(b).build();
        return false;
    }

    const std::vector<CoveringSymbol> covers = covering_symbols(*exe_obj);
    const bool macho = llvm::isa<llvm::object::MachOObjectFile>(exe_obj);

    llvm::DILineInfoSpecifier spec(
        llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
        llvm::DILineInfoSpecifier::FunctionNameKind::ShortName
    );

    b.begin_symbolize();

    for (size_t i = 0; i < b.frames().size(); i++) {
        const uint64_t runtime = b.frames()[i];
        const uint64_t file_addr = runtime >= slide ? runtime - slide : runtime;

        llvm::object::SectionedAddress address;
        address.Address = file_addr;
        address.SectionIndex = llvm::object::SectionedAddress::UndefSection;

        llvm::DIInliningInfo info = dwarf->getInliningInfoForAddress(address, spec);
        const CoveringSymbol *cover = covering_at(covers, file_addr);

        const uint32_t first = static_cast<uint32_t>(b.inlines().size());
        uint32_t count = 0;

        auto push = [&](std::string function, std::string source, uint32_t line, uint32_t column) {
            if (function.empty()) {
                return;
            }
            ProfileInline rec;
            rec.function = b.intern_string(function);
            rec.file = b.intern_string(source);
            rec.line = line;
            rec.column = column;
            b.add_inline(rec);
            count++;
        };

        if (info.getNumberOfFrames() == 0) {
            if (cover != nullptr) {
                push(macho ? strip_macho_underscore(cover->name) : cover->name, "", 0, 0);
            }
        } else {
            for (uint32_t f = 0; f < info.getNumberOfFrames(); f++) {
                const llvm::DILineInfo &frame = info.getFrame(f);
                std::string function;
                if (named(frame)) {
                    function = frame.FunctionName;
                } else if (cover != nullptr && f == 0) {
                    function = macho ? strip_macho_underscore(cover->name) : cover->name;
                }
                std::string source = frame.FileName == llvm::DILineInfo::BadString
                    ? std::string()
                    : frame.FileName;
                push(std::move(function), std::move(source), frame.Line, frame.Column);
            }
        }

        b.set_addr(i, first, count);
    }

    b.mark_symbolized();
    file = std::move(b).build();
    return true;
}
