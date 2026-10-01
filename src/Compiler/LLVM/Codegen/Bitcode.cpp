#include "Compiler/LLVM/Codegen/Bitcode.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/raw_ostream.h>

std::string Compiler::LLVM::bitcode_of(llvm::Module &module)
{
    llvm::SmallString<0> buffer;
    llvm::raw_svector_ostream stream(buffer);
    llvm::WriteBitcodeToFile(module, stream);
    return std::string(buffer.data(), buffer.size());
}
