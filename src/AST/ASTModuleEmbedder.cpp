#include "AST/ASTModuleEmbedder.h"
#include "AST/ASTModule.h"
#include "Compiler/BuildLayout.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace
{

std::string with_forward_slashes(std::string path)
{
    for (char &c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    return path;
}

std::string as_c_string_literal(const std::string &value)
{
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += static_cast<char>(c);
                break;
        }
    }
    out += '"';
    return out;
}

bool should_skip_stdlib_directory(const std::filesystem::path &name)
{
    const std::string leaf = name.filename().string();
    return leaf == "build" || leaf == "sketches";
}

void write_byte_array(std::ostream &output, const std::string &varname, const std::string &content)
{
    output << fmt::format("static const unsigned char {}_data[] = {{\n", varname);
    output << "        ";
    for (size_t i = 0; i < content.size(); ++i) {
        if (i > 0 && i % 12 == 0) {
            output << "\n        ";
        }
        output << "0x" << std::hex << std::setw(2) << std::setfill('0')
            << (static_cast<unsigned int>(static_cast<unsigned char>(content[i]))) << ", ";
    }
    output << std::dec;
    output << "\n    };\n";
}

void open_for_writing(std::ofstream &output, const std::string &path)
{
    output.open(path);
    if (!output.is_open()) {
        throw std::runtime_error(fmt::format("cannot open '{}' for writing", path));
    }
}

};

std::string AST::embedded_relative_path(const std::filesystem::path &path)
{
    std::string file_path = with_forward_slashes(path.generic_string());
    std::string stdlib_root = with_forward_slashes(
        std::filesystem::path(STDLIB_SOURCE_DIR).generic_string());

    while (!stdlib_root.empty() && stdlib_root.back() == '/') {
        stdlib_root.pop_back();
    }

    if (file_path.size() >= stdlib_root.size()
        && file_path.compare(0, stdlib_root.size(), stdlib_root) == 0
        && (file_path.size() == stdlib_root.size() || file_path[stdlib_root.size()] == '/')) {
        std::string rel = file_path.substr(stdlib_root.size());
        if (!rel.empty() && rel.front() == '/') {
            rel.erase(rel.begin());
        }
        return rel;
    }

    return {};
}

std::string AST::embedded_source_path(const std::filesystem::path &path)
{
    const std::string rel = embedded_relative_path(path);
    if (!rel.empty()) {
        return Compiler::embedded_stdlib_path(rel).generic_string();
    }

    return with_forward_slashes(path.generic_string());
}

std::vector<std::pair<std::string, std::string>> AST::collect_stdlib_files(const std::filesystem::path &stdlib_root)
{
    std::vector<std::pair<std::string, std::string>> files;
    std::error_code ec;

    const auto add_file = [&files, &stdlib_root](const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error(fmt::format("cannot read stdlib file '{}'", path.string()));
        }
        const std::string content(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );
        std::string rel = with_forward_slashes(
            std::filesystem::relative(path, stdlib_root).generic_string());
        files.emplace_back(std::move(rel), content);
    };

    const std::filesystem::path manifest = stdlib_root / "module.eco";
    if (!std::filesystem::is_regular_file(manifest, ec)) {
        throw std::runtime_error(fmt::format(
            "stdlib root '{}' has no module.eco", stdlib_root.string()));
    }
    add_file(manifest);

    for (auto it = std::filesystem::recursive_directory_iterator(stdlib_root, ec);
        it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) {
            break;
        }

        if (it->is_directory(ec)) {
            if (should_skip_stdlib_directory(it->path())) {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!it->is_regular_file(ec) || it->path().extension() != ".eco") {
            continue;
        }

        if (it->path().filename() == "module.eco" && it->path().parent_path() == stdlib_root) {
            continue;
        }

        add_file(it->path());
    }

    std::sort(files.begin(), files.end(),
        [](const std::pair<std::string, std::string> &a,
            const std::pair<std::string, std::string> &b) {
            return a.first < b.first;
        });

    return files;
}

void AST::write_embedded_module(AST::Module &module, const std::string &output_path)
{
    std::ofstream output;
    open_for_writing(output, output_path);

    output << "#include \"AST/ASTModule.h\"\n\n";
    output << "#include \"AST/ASTBundle.h\"\n\n";

    output << "namespace EmbeddedModule\n{\n\n";

    output << fmt::format("void load_{}_module(AST::Bundle &bundle, AST::Module &module)\n", module.name);
    output << "{\n";

    int file_index = 0;
    for (auto &file : module.files()) {
        file_index++;
        std::string filevar = fmt::format("file_{}", file_index);
        std::string file_path = embedded_source_path(file.get_path());

        output << fmt::format("    auto &{} = module.add_file({});\n", filevar, as_c_string_literal(file_path));

        write_byte_array(output, filevar, file.content.value_or(""));
        output << fmt::format(
            "    {}.set_content(reinterpret_cast<const char*>({}_data), sizeof({}_data));\n",
            filevar,
            filevar,
            filevar
        );
    }

    output << "}\n";
    output << "}\n";

    output.close();
}

void AST::write_embedded_stdlib(const std::filesystem::path &stdlib_root, const std::string &output_path)
{
    const auto files = collect_stdlib_files(stdlib_root);

    std::ofstream output;
    open_for_writing(output, output_path);

    output << "#include <cstddef>\n";
    output << "#include <filesystem>\n";
    output << "#include <optional>\n";
    output << "#include <string>\n";
    output << "#include <string_view>\n";
    output << "#include <vector>\n\n";

    output << "namespace EmbeddedModule\n{\n\n";

    output << "struct File\n{\n";
    output << "    const char *path;\n";
    output << "    const unsigned char *data;\n";
    output << "    size_t size;\n";
    output << "};\n\n";

    for (size_t i = 0; i < files.size(); ++i) {
        write_byte_array(output, fmt::format("file_{}", i + 1), files[i].second);
        output << "\n";
    }

    output << "static const File k_files[] = {\n";
    for (size_t i = 0; i < files.size(); ++i) {
        output << fmt::format(
            "    {{ {}, file_{}_data, sizeof(file_{}_data) }},\n",
            as_c_string_literal(files[i].first),
            i + 1,
            i + 1
        );
    }
    output << "};\n\n";

    output << "inline constexpr size_t k_file_count = sizeof(k_files) / sizeof(k_files[0]);\n\n";

    output << "inline std::string_view relative_of(std::string_view path)\n";
    output << "{\n";
    output << "    constexpr std::string_view prefix = \""
        << Compiler::k_embedded_stdlib_scheme << "/\";\n";
    output << "    if (path.size() >= prefix.size() && path.substr(0, prefix.size()) == prefix) {\n";
    output << "        return path.substr(prefix.size());\n";
    output << "    }\n";
    output << "    return path;\n";
    output << "}\n\n";

    output << "inline std::optional<std::string> content_of(const std::filesystem::path &path)\n";
    output << "{\n";
    output << "    const std::string spelled = path.generic_string();\n";
    output << "    const std::string_view rel = relative_of(spelled);\n";
    output << "    for (size_t i = 0; i < k_file_count; i++) {\n";
    output << "        if (rel == k_files[i].path) {\n";
    output << "            return std::string(\n";
    output << "                reinterpret_cast<const char *>(k_files[i].data), k_files[i].size);\n";
    output << "        }\n";
    output << "    }\n";
    output << "    return std::nullopt;\n";
    output << "}\n\n";

    output << "inline std::vector<std::string> relative_paths()\n";
    output << "{\n";
    output << "    std::vector<std::string> out;\n";
    output << "    out.reserve(k_file_count);\n";
    output << "    for (size_t i = 0; i < k_file_count; i++) {\n";
    output << "        out.emplace_back(k_files[i].path);\n";
    output << "    }\n";
    output << "    return out;\n";
    output << "}\n\n";

    output << "inline std::optional<std::string> manifest_text()\n";
    output << "{\n";
    output << "    return content_of(\"module.eco\");\n";
    output << "}\n\n";

    output << "}\n";
    output.close();
}
