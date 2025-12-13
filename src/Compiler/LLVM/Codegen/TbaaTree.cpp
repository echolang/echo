#include "Compiler/LLVM/Codegen/TbaaTree.h"

#include "AST/ASTAccessFamily.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>

namespace Compiler::LLVM
{

TbaaTree::TbaaTree(llvm::LLVMContext &context) : _builder(context)
{
    _root = _builder.createTBAARoot("Echo TBAA");

    // **the ancestor of every typed leaf, not a sibling of them.** a `uint8` access has to alias
    // everything, because a byte-wise view of memory is a thing this language spells - and a sibling
    // node would let a byte write be reordered past a typed one, which is the classic way to get this
    // wrong
    _byte = _builder.createTBAANode("byte", _root);
}

llvm::MDNode *TbaaTree::leaf(const std::string &name)
{
    auto it = _leaves.find(name);
    if (it != _leaves.end()) {
        return it->second;
    }

    // the *access tag*, not the scalar node: an instruction's `!tbaa` operand is a three-element tag
    // (base, access, offset), and handing it the bare scalar node is the mistake that makes LLVM
    // read the metadata as malformed and drop it in silence
    llvm::MDNode *scalar = _builder.createTBAANode(name, _byte);
    llvm::MDNode *tag = _builder.createTBAAStructTagNode(scalar, scalar, 0);

    _leaves[name] = tag;
    return tag;
}

llvm::MDNode *TbaaTree::scalar_tag(const AST::ValueType &type)
{
    const AST::AccessFamily family = AST::access_family_of(type);

    // **no family means emit nothing**, which is the conservative answer rather than a missing one:
    // an untagged instruction may alias anything
    if (family == AST::AccessFamily::t_none) {
        return nullptr;
    }

    // **the byte family *is* the ancestor node, not a leaf under it.** `int8`, `uint8` and `bool`
    // answer it, and the whole point is that they then alias every typed family below - which is
    // what makes looking at an object's bytes mean what it says
    if (family == AST::AccessFamily::t_byte) {
        return byte_tag();
    }

    // every other family is one leaf directly under `byte`, named from the one table. there is
    // deliberately no per-*type* leaf: `int32` and `uint32` are two readings of one bit pattern, and
    // separating them here would claim something AST::access_families_may_alias does not
    return leaf(AST::access_family_name(family));
}

llvm::MDNode *TbaaTree::byte_tag()
{
    if (_byte_tag == nullptr) {
        _byte_tag = _builder.createTBAAStructTagNode(_byte, _byte, 0);
    }

    return _byte_tag;
}

llvm::MDNode *TbaaTree::runtime_tag(const std::string &name)
{
    return leaf("runtime." + name);
}

};
