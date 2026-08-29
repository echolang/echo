#include "AST/ASTModule.h"

#include "AST/ASTBundle.h"

namespace EmbeddedModule
{

void load_stdlib_module(AST::Bundle &bundle, AST::Module &module)
{
    auto &file_1 = module.add_file("stdlib:/core/ordered_map.eco");
    static const unsigned char file_1_data[] = {
        0x66, 0x75, 0x6e, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x66, 0x28, 0x29, 
        0x20, 0x7b, 0x7d, 0x0a, 
    };
    file_1.set_content(reinterpret_cast<const char*>(file_1_data), sizeof(file_1_data));
}
}
