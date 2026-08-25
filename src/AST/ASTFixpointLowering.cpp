#include "AST/ASTFixpointLowering.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTRegion.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"

namespace AST
{

FixpointLowering::FixpointLowering(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef FixpointLowering::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, token.make_slice()};
}

size_t FixpointLowering::next_hoist_index()
{
    assert_region_accepts_mutation(_current_function, _current_file);
    return _hoist_count++;
}

bool FixpointLowering::run_round()
{
    _changed = false;

    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            _current_file = &file;

            // the hoist numbering restarts here, and this line is the whole of the per-file rule: a
            // name minted in one file cannot be renumbered by an unrelated file above it growing one
            _hoist_count = 0;

            if (file.root != nullptr) {
                file.root->accept(*this);
            }
        }
    }

    return _changed;
}

void FixpointLowering::finalize()
{
    // one more round rather than a sweep: a round inherits visitFunctionDecl's generic-body skip and
    // walks scope children, which is the tree walk these passes are required to use - NodeCollection
    // owns a detached node forever, so an of_type sweep would blame nodes that were lowered away
    _finalizing = true;
    run_round();
    _finalizing = false;
}

void FixpointLowering::visitFunctionDecl(FunctionDeclNode &node)
{
    if (node.is_generic()) {
        return;
    }

    FunctionDeclNode *enclosing = _current_function;
    _current_function = &node;
    RecursiveVisitor::visitFunctionDecl(node);
    _current_function = enclosing;
}

};  // namespace AST
