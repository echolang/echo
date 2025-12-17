#ifndef DEBUGINFOCODEGEN_H
#define DEBUGINFOCODEGEN_H

#pragma once

#include "AST/ASTValueType.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AST
{
    class File;
    class Node;
    class FunctionDeclNode;
    class VarDeclNode;
};

namespace llvm
{
    class StructLayout;
};

namespace Compiler::LLVM
{
    struct CodegenContext;
    struct CmpUnit;

    // **the whole of what a debugger is told about a program.**
    //
    // every entry point below is a no-op when `-g` is off, which is what keeps its ~thirty call sites
    // free of a flag check - the same shape TbaaTree has for --no-tbaa. Nothing here changes what the
    // program *does*; it only changes what an object says about itself.
    //
    // **one DIBuilder per CmpUnit, and that is the opposite of TbaaTree.** The two look alike and are
    // not. A TBAA node's identity *is* its `MDNode *`, so two units emitting `int32` accesses must share
    // one leaf or LLVM reads two equal nodes as unrelated - hence one tree per LLVMContext. A DIBuilder
    // is constructed over an llvm::Module, appends its DICompileUnit to *that* module's `llvm.dbg.cu`,
    // and its finalize() resolves the temporary nodes it made and no others. One shared across units
    // would give one module a compile unit and leave the rest holding orphan subprograms the verifier
    // rejects. Debug identity across units is instead ODR-string identity, which is what the
    // `identifier:` on a named composite is for - so a per-unit DIType cache is right for the same
    // reason CmpUnit::structure_table is per unit.
    class DebugInfoCodegen
    {
    public:
        DebugInfoCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // the compile unit and the module flags, minted where the llvm::Module is - beside its data
        // layout and triple, because all three are things a module carries from the moment it exists
        void create_unit(CmpUnit &cmp_unit);

        // **the subprogram, attached at the definition and never at Function::Create.** A unit that only
        // references a symbol declares it, and LLVM's verifier refuses a !dbg attachment on a
        // declaration - so only the unit supplying the body may attach one. Same reason, same place, as
        // gen_function_decl's flip to linkonce_odr
        void begin_function(const AST::FunctionDeclNode &node, llvm::Function *func);

        // the entry point, which has no FunctionDeclNode at all - LLVMCompiler::compile_bundle builds it
        // with a bare Function::Create
        void begin_entry_point(llvm::Function *func);

        void end_function();

        // **`main`'s body is the concatenation of every file root of the entry module**, so a location
        // from the second file would sit inside a subprogram whose file is the first. That is illegal to
        // describe with a plain scope, and this is the shape that describes it - the same thing a C
        // compiler emits for an #included body. Balanced by pop_file_scope
        void push_file_scope(const AST::File *file);
        void pop_file_scope();

        // **a block's own scope, so two locals of one name in sibling blocks resolve apart.** Pushed by
        // StmtCodegen::gen_scope around a scope that carries an opening brace, which is exactly the
        // scopes a person wrote - a body's own scope and every scope a pass minted share the
        // subprogram's, which is where their declarations belong anyway.
        //
        // push answers whether it pushed anything, and pop must be handed that answer back: the two
        // sit in one C++ stack frame, so the caller is what knows, and a pop that assumed otherwise
        // would eat the enclosing block's scope
        bool push_lexical_block(const AST::Node &scope);
        void pop_lexical_block(bool pushed);

        // **the statement seam.** set once per statement in StmtCodegen::gen_scope, and inherited by
        // every load, call and branch the subtree below produces - which is what makes a line table out
        // of a walk that knows nothing about lines.
        //
        // a node with no token of its own keeps the location already set rather than clearing it: an
        // instruction inside a subprogram may not be *unlocated* once its neighbours are, and the
        // enclosing statement is the honest answer for a scope or a synthesized release anyway
        void set_location(const AST::Node &node);

        // the subprogram's own line, for the two moments that are not a statement: the parameter spill
        // stores, whose location is what makes LLVM place `prologue_end` at the first real line, and the
        // synthesized terminator a body without an explicit return gets
        void set_function_scope_location();

        void clear_location();

        // **what the location should be now that the builder stands in `block`.** Called only by
        // CodegenContext::set_insert_point, which is the one place the builder moves.
        //
        // an instruction's `!dbg` scope must belong to the subprogram of the function it sits in, so
        // this is a *decision* rather than a restore: moving inside the body being emitted re-applies
        // the statement's location, and moving anywhere else clears it. The second half is what the
        // emitted runtime needs - `__eco_alloc`, `__eco_abort`, the reference-count operations are all
        // built mid-body, have no AST declaration and therefore no subprogram, and were inheriting the
        // location of whichever Echo statement happened to materialize them into that unit first. Which
        // made a linkonce_odr body differ between units, and pointed the runtime's line table at a
        // source line it has nothing to do with
        void relocate(llvm::BasicBlock *block);

        // a parameter or a local, at the alloca holding it. `arg_no` is 1-based and set for a parameter
        // only, which is the whole of what distinguishes the two in DWARF
        void declare_local(
            llvm::AllocaInst *alloca,
            const AST::VarDeclNode &decl,
            std::optional<unsigned> arg_no
        );

        // the type map, mirroring TypeLowering::get_llvm_type arm for arm - it is the same taxonomy
        // answered in a different vocabulary, and keeping the two the same shape is what keeps them in
        // step. null is legal everywhere a DIType can appear, and means "not inspectable"
        llvm::DIType *type_of(const AST::ValueType &type, CmpUnit &cmp_unit);

        // every unit's builder, once every body is in. **after drain_pending_definitions**, which is the
        // last thing that can add a body, and **before verify_odr_consistency**, which must see final
        // metadata. Everything downstream - the merge, the pipelines, the object writer - is then
        // strictly later, which is the whole reason there is one moment rather than a rule per consumer
        void finalize_all();

    private:

        // is this build emitting anything at all. every entry point answers it itself, which is what
        // keeps the flag out of the call sites
        bool enabled() const;

        struct UnitDebug
        {
            std::unique_ptr<llvm::DIBuilder> builder;
            llvm::DICompileUnit *cu = nullptr;
            std::unordered_map<const AST::File *, llvm::DIFile *> files;

            // keyed by ValueType, which compares by pointer identity on the ComplexType - so two
            // instantiations are two entries, which is exactly right
            std::unordered_map<AST::ValueType, llvm::DIType *> types;
        };

        // keyed by unit rather than living on CmpUnit, because with `-g` off there is nothing here at
        // all - an empty map is the honest representation of that, where a null unique_ptr on every
        // CmpUnit would be a field every reader has to remember to check
        std::unordered_map<const CmpUnit *, UnitDebug> _units;

        UnitDebug *unit_debug();
        UnitDebug *unit_debug(const CmpUnit &cmp_unit);

        llvm::DIFile *file_for(UnitDebug &unit, const AST::File *file);

        llvm::DISubroutineType *subroutine_type_of(
            const AST::FunctionDeclNode &node,
            CmpUnit &cmp_unit
        );

        // the composite shapes, each interned through a replaceable temporary *before* its members are
        // built - a struct holding a pointer to itself is otherwise an infinite recursion rather than a
        // wrong answer
        llvm::DIType *struct_type_of(const AST::ValueType &type, CmpUnit &cmp_unit);
        llvm::DIType *class_type_of(const AST::ValueType &type, CmpUnit &cmp_unit);
        // a type's own properties as DIMemberTypes, appended at their offsets in `layout` shifted by
        // `base_offset_bits` - zero for a struct, the payload's offset in the box for a class, which
        // is the whole of what separates the two
        void append_property_members(
            UnitDebug &unit,
            const AST::ComplexType *complex,
            CmpUnit &cmp_unit,
            llvm::DIFile *decl_file,
            unsigned decl_line,
            const llvm::StructLayout &layout,
            size_t element_count,
            uint64_t base_offset_bits,
            std::vector<llvm::Metadata *> &elements
        );

        llvm::DIType *tuple_type_of(
            const AST::ValueType &type,
            CmpUnit &cmp_unit,
            const std::string &name,
            const std::vector<std::pair<std::string, llvm::DIType *>> &members,
            llvm::StructType *llvm_struct
        );

        // **the scope chain of the function being emitted, innermost last.** shaped like
        // CodegenContext::loop_targets and for its reason: one owner is what keeps two emitters from
        // disagreeing about which scope is innermost
        std::vector<llvm::DIScope *> _scopes;

        llvm::DISubprogram *_subprogram = nullptr;
        llvm::DebugLoc _location;

        llvm::DIScope *current_scope() const;

        CodegenContext &_ctx;
    };
};

#endif
