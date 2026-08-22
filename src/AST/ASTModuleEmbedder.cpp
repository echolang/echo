#include "AST/ASTModuleEmbedder.h"
#include "AST/ASTModule.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

namespace
{

// `path.generic_string()` is not enough: on POSIX a string built with backslashes
// is one filename, and the Windows release path is native `\` against a CMake
// STDLIB_SOURCE_DIR that uses `/`. one spelling, so the prefix match cannot miss
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

};

std::string AST::embedded_source_path(const std::filesystem::path &path)
{
    std::string file_path = with_forward_slashes(path.generic_string());
    std::string stdlib_root = with_forward_slashes(
        std::filesystem::path(STDLIB_SOURCE_DIR).generic_string());

    // drop a trailing slash so the remainder keeps its leading one, matching the
    // existing `stdlib:/core/...` spelling
    while (!stdlib_root.empty() && stdlib_root.back() == '/') {
        stdlib_root.pop_back();
    }

    if (file_path.size() >= stdlib_root.size()
        && file_path.compare(0, stdlib_root.size(), stdlib_root) == 0
        && (file_path.size() == stdlib_root.size() || file_path[stdlib_root.size()] == '/')) {
        file_path = "stdlib:" + file_path.substr(stdlib_root.size());
    }

    return file_path;
}

void AST::write_embedded_module(AST::Module &module, const std::string &output_path)
{
    std::ofstream output(output_path);
    if (!output.is_open()) {
        throw std::runtime_error(fmt::format("Failed to embedding file for writing: {}", output_path));
    }

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

        output << fmt::format("    static const unsigned char {}_data[] = {{\n", filevar);
        output << "        ";
        auto content = file.content.value_or("");
        for (size_t i = 0; i < content.size(); ++i) {
            if (i > 0 && i % 12 == 0) {
                output << "\n        ";
            }
            output << "0x" << std::hex << std::setw(2) << std::setfill('0') << (static_cast<unsigned int>(content[i]) & 0xff) << ", ";
        }
        output << "\n    };\n";
        output << fmt::format("    {}.set_content(reinterpret_cast<const char*>({}_data), sizeof({}_data));\n", filevar, filevar, filevar);
    }

    output << "}\n";
    output << "}\n";

    output.close();
}
