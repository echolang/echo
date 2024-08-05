#include "AST/ASTModuleEmbedder.h"

#include <fstream>
#include <fmt/format.h>
#include <AST/ASTModule.h>

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

        // this is not really clean, but it works for now 
        // we can take the "STDLIB_SOURCE_DIR" define to determine the relative path
        // of the full file path if it begins with it
        std::string file_path = file.get_path().string();
        if (file_path.find(STDLIB_SOURCE_DIR) == 0) {
            file_path = "stdlib:" + file_path.substr(strlen(STDLIB_SOURCE_DIR));
        }

        output << fmt::format("    auto &{} = module.add_file(\"{}\");\n", filevar, file_path);

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