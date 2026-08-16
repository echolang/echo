#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/Codegen/ClassLayout.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/CompilationUnit.h"

#include "eco.h"

#include "AST/ASTFile.h"
#include "AST/ASTMangler.h"
#include "AST/ASTModule.h"
#include "AST/ASTSourceToken.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Verifier.h>
#include <llvm/TargetParser/Triple.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{

namespace
{
    // **DWARF 4 on Darwin, 5 everywhere else.** One owner and not a guess per call site, for
    // resolve_subtarget's reason - and the value has to be identical in every unit, or
    // llvm::Linker::linkInModule refuses the merge over a module-flag mismatch.
    //
    // 4 on Darwin because that is what the platform's linker writes a debug map for and what dsymutil
    // and the shipped lldb are built around; 5 elsewhere is LLVM's own default and what every current
    // ELF consumer reads.
    //
    // asked of the **target** triple and not of `__APPLE__`: this decides what goes into the object,
    // where Backend's own #if defined(__APPLE__) decides which host tool to invoke
    unsigned dwarf_version_for(const std::string &triple)
    {
        return llvm::Triple(triple).isOSDarwin() ? 4 : 5;
    }

    // what a debugger calls this kind of value. the *width* is deliberately not here -
    // AST::get_primitive_size owns it, and TypeLowering picks its llvm width off the same answer, so
    // a third table is a third thing to drift. 0 is "no encoding", which is void's answer
    unsigned basic_type_encoding(const AST::ValueType &type)
    {
        if (type.is_boolean_type()) {
            return llvm::dwarf::DW_ATE_boolean;
        }

        if (type.is_floating_type()) {
            return llvm::dwarf::DW_ATE_float;
        }

        if (type.is_signed_integer()) {
            return llvm::dwarf::DW_ATE_signed;
        }

        return type.is_unsigned_integer() ? llvm::dwarf::DW_ATE_unsigned : 0;
    }

    // **lldb parses a leading `$` as one of its own convenience variables**, so a DWARF name of `$count`
    // is a name the `p` command cannot reach. Echo's own accessors already exclude the sigil - it is
    // what `arg.setName` uses - so this only guards the day one of them stops doing so
    std::string debugger_name(const std::string &name)
    {
        return !name.empty() && name.front() == '$' ? name.substr(1) : name;
    }

    // an address whose pointee is deliberately not described - a vtable, a closure environment, the
    // type-info word. `p` still reads the address, which is the whole of what any of them is for
    llvm::DIType *opaque_pointer(llvm::DIBuilder &builder)
    {
        return builder.createPointerType(nullptr, ECO_TARGET_POINTER_SIZE * 8);
    }
};

bool DebugInfoCodegen::enabled() const
{
    return _ctx.options.emitting_debug_info();
}

DebugInfoCodegen::UnitDebug *DebugInfoCodegen::unit_debug(const CmpUnit &cmp_unit)
{
    auto found = _units.find(&cmp_unit);
    return found != _units.end() ? &found->second : nullptr;
}

DebugInfoCodegen::UnitDebug *DebugInfoCodegen::unit_debug()
{
    return _ctx.current_cmp_unit != nullptr ? unit_debug(*_ctx.current_cmp_unit) : nullptr;
}

void DebugInfoCodegen::create_unit(CmpUnit &cmp_unit)
{
    if (!enabled() || cmp_unit.llvm_module == nullptr) {
        return;
    }

    llvm::Module &module = *cmp_unit.llvm_module;

    // **without this flag LLVM silently *strips* every location** rather than reporting anything, which
    // reads as "the compiler emitted nothing". The second one is what the object writer emits against.
    // Both merge cleanly across units because every unit computes them the same way
    module.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
    module.addModuleFlag(llvm::Module::Max, "Dwarf Version", dwarf_version_for(_ctx.target_triple));

    UnitDebug &unit = _units[&cmp_unit];
    unit.builder = std::make_unique<llvm::DIBuilder>(module);

    // a compile unit needs a file even though an Echo module holds several; the first is the one the
    // rest hang off, and each body names its own through its subprogram
    AST::File *first = cmp_unit.ast_module->files().first();

    llvm::DIFile *cu_file = first != nullptr
        ? file_for(unit, first)
        : unit.builder->createFile(cmp_unit.ast_module->name, ".");

    // **DW_LANG_C_plus_plus, because there is no code for Echo.** It is the closest thing a consumer
    // already understands - structs with methods, references, mangled linkage names - and picking one
    // nothing recognises would cost identifier demangling and member display for no gain.
    //
    // isOptimized is set *truthfully*: a false claim over an optimized build is what stops lldb warning
    // about a value it should warn about, and leaves a person reading `<optimized out>` as a bug
    unit.cu = unit.builder->createCompileUnit(
        llvm::dwarf::DW_LANG_C_plus_plus,
        cu_file,
        "echoc",
        /*isOptimized=*/!_ctx.options.no_optimize,
        /*Flags=*/"",
        /*RV=*/0);
}

llvm::DIFile *DebugInfoCodegen::file_for(UnitDebug &unit, const AST::File *file)
{
    auto found = unit.files.find(file);

    if (found != unit.files.end()) {
        return found->second;
    }

    // the **full path**, split the way DWARF wants it - unlike an abort message, which prints only
    // the file name so a golden stays machine-independent. A debugger has to find the source on
    // disk, so it needs the directory the goldens must never see.
    //
    // **made absolute**, which is not cosmetic: a manifest's sources already arrive absolute, but a
    // loose file named on the command line arrives exactly as it was typed, so `echoc build -g test.eco`
    // recorded `directory: "."`. Breakpoints and variables still worked and the *source listing* did
    // not - a debugger resolves that against a working directory nothing recorded, so stepping was
    // blind the moment it ran from anywhere else. std::filesystem::absolute is the cwd this compile
    // ran in, which is what DW_AT_comp_dir means everywhere else too
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(file->get_path(), ec);

    if (ec) {
        path = file->get_path();
    }

    llvm::DIFile *di_file = unit.builder->createFile(
        path.filename().string(),
        path.has_parent_path() ? path.parent_path().string() : std::string("."));

    unit.files[file] = di_file;

    return di_file;
}

llvm::DIScope *DebugInfoCodegen::current_scope() const
{
    return _scopes.empty() ? nullptr : _scopes.back();
}

llvm::DISubroutineType *DebugInfoCodegen::subroutine_type_of(
    const AST::FunctionDeclNode &node,
    CmpUnit &cmp_unit
)
{
    UnitDebug *unit = unit_debug(cmp_unit);

    // the return type first, then the parameters - that is the order DWARF reads the array in, and a
    // null first entry is how `void` is spelled
    std::vector<llvm::Metadata *> signature;
    signature.push_back(type_of(node.get_return_type(), cmp_unit));

    for (const AST::VarDeclNode *arg : node.args) {
        signature.push_back(arg->has_type() ? type_of(arg->type_node()->type, cmp_unit) : nullptr);
    }

    return unit->builder->createSubroutineType(unit->builder->getOrCreateTypeArray(signature));
}

void DebugInfoCodegen::begin_function(const AST::FunctionDeclNode &node, llvm::Function *func)
{
    UnitDebug *unit = unit_debug();

    if (unit == nullptr || func == nullptr) {
        return;
    }

    // **the declaration's own file, never the ambient one.** A t_odr_shared body is emitted into every
    // unit that references it, so N units mint N subprograms for one symbol and they have to agree.
    //
    // the *token* first and CodegenContext::function_file_map second, which is the opposite of an
    // optimization: file_of falls back to the ambient `current_file` on a miss, and a miss is silent -
    // so a body reached through a gap in that map got the file of whichever unit was being lowered, and
    // the two objects disagreed. A token names one collection, and one module owns it, in every unit
    const TokenReference *token = AST::source_token_of(node);

    AST::File *file = token != nullptr ? token->file() : nullptr;

    if (file == nullptr) {
        file = _ctx.file_of(&node);
    }

    if (file == nullptr) {
        return;
    }

    llvm::DIFile *di_file = file_for(*unit, file);

    // an instantiation shares its template's line, because the code really was written there - the same
    // thing C++ template debug info does. What tells two instances apart is the linkage name
    const unsigned line = token != nullptr ? token->line() : 1;

    llvm::DISubprogram::DISPFlags flags = llvm::DISubprogram::SPFlagDefinition;

    if (!_ctx.options.no_optimize) {
        flags |= llvm::DISubprogram::SPFlagOptimized;
    }

    _subprogram = unit->builder->createFunction(
        di_file,
        debugger_name(node.func_name()),
        // the symbol this body is being emitted under, read off the function rather than mangled a
        // second time - gen_function_decl already did that walk to find this very llvm::Function
        func->getName(),
        di_file,
        line,
        subroutine_type_of(node, *_ctx.current_cmp_unit),
        /*ScopeLine=*/line,
        llvm::DINode::FlagPrototyped,
        flags);

    func->setSubprogram(_subprogram);
    _scopes.push_back(_subprogram);
}

void DebugInfoCodegen::begin_entry_point(llvm::Function *func)
{
    UnitDebug *unit = unit_debug();

    if (unit == nullptr || func == nullptr || unit->cu == nullptr) {
        return;
    }

    llvm::DIFile *di_file = unit->cu->getFile();

    _subprogram = unit->builder->createFunction(
        di_file,
        "main",
        ECO_ENTRY_SYMBOL_NAME,
        di_file,
        /*LineNo=*/1,
        unit->builder->createSubroutineType(unit->builder->getOrCreateTypeArray({})),
        /*ScopeLine=*/1,
        llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::SPFlagDefinition);

    func->setSubprogram(_subprogram);
    _scopes.push_back(_subprogram);
}

void DebugInfoCodegen::end_function()
{
    if (_subprogram == nullptr) {
        return;
    }

    _scopes.clear();
    _subprogram = nullptr;

    // nothing may leak into whatever is emitted next: an instruction outside any subprogram carrying a
    // location that names one is a verifier error
    clear_location();
}

void DebugInfoCodegen::push_file_scope(const AST::File *file)
{
    UnitDebug *unit = unit_debug();

    if (unit == nullptr || _subprogram == nullptr || file == nullptr) {
        return;
    }

    _scopes.push_back(unit->builder->createLexicalBlockFile(current_scope(), file_for(*unit, file)));
}

void DebugInfoCodegen::pop_file_scope()
{
    // never past the subprogram itself, which end_function owns
    if (_scopes.size() > 1) {
        _scopes.pop_back();
    }
}

bool DebugInfoCodegen::push_lexical_block(const AST::Node &scope)
{
    UnitDebug *unit = unit_debug();

    if (unit == nullptr || _subprogram == nullptr) {
        return false;
    }

    const TokenReference *brace = AST::source_token_of(scope);

    // no brace means nothing wrote this block, so there is no block to describe - its declarations
    // belong to the enclosing scope, which is what leaving the stack alone gives them
    if (brace == nullptr) {
        return false;
    }

    _scopes.push_back(unit->builder->createLexicalBlock(
        current_scope(), _subprogram->getFile(), brace->line(), brace->column()));

    return true;
}

void DebugInfoCodegen::pop_lexical_block(bool pushed)
{
    // told by the caller rather than recorded here, because push declines for a scope nobody wrote -
    // and a pop that assumed it had pushed would eat the enclosing block's scope instead. The two
    // always sit in one C++ stack frame, so the caller is the thing that knows
    if (!pushed || _scopes.empty()) {
        return;
    }

    _scopes.pop_back();
}

void DebugInfoCodegen::set_location(const AST::Node &node)
{
    if (_subprogram == nullptr) {
        return;
    }

    const TokenReference *token = AST::source_token_of(node);

    // **a node with no token keeps whatever was already set.** Clearing it would leave an instruction
    // inside a subprogram unlocated, which the verifier refuses once its neighbours are located - and
    // the enclosing statement is the honest position for a scope or a synthesized release anyway.
    //
    // a token AST::Module::make_virtual_token minted needs no arm here: it carries the real line and
    // column of the site it was minted at, and its *file* - the one thing it cannot answer - is not
    // part of a DILocation, which takes that from the scope
    if (token == nullptr) {
        return;
    }

    _location = llvm::DILocation::get(
        *_ctx.llvm_context, token->line(), token->column(), current_scope());

    _ctx.builder->SetCurrentDebugLocation(_location);
}

void DebugInfoCodegen::set_function_scope_location()
{
    if (_subprogram == nullptr) {
        return;
    }

    _location = llvm::DILocation::get(
        *_ctx.llvm_context, _subprogram->getScopeLine(), 0, _subprogram);

    _ctx.builder->SetCurrentDebugLocation(_location);
}

void DebugInfoCodegen::clear_location()
{
    _location = llvm::DebugLoc();

    if (_ctx.builder) {
        _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());
    }
}

void DebugInfoCodegen::relocate(llvm::BasicBlock *block)
{
    if (!enabled()) {
        return;
    }

    // the same function this location's scope belongs to, or nothing. compared against the subprogram
    // rather than merely tested for presence, so stepping into *another* body's blocks is treated the
    // same way as stepping into the runtime's - which it is
    const bool inside_current_body = block != nullptr
        && block->getParent() != nullptr
        && _subprogram != nullptr
        && block->getParent()->getSubprogram() == _subprogram;

    _ctx.builder->SetCurrentDebugLocation(
        inside_current_body ? _location : llvm::DebugLoc());
}

void DebugInfoCodegen::declare_local(
    llvm::AllocaInst *alloca,
    const AST::VarDeclNode &decl,
    std::optional<unsigned> arg_no
)
{
    UnitDebug *unit = unit_debug();

    if (unit == nullptr || _subprogram == nullptr || alloca == nullptr || !decl.has_type()) {
        return;
    }

    llvm::DIType *type = type_of(decl.type_node()->type, *_ctx.current_cmp_unit);

    if (type == nullptr) {
        return;
    }

    llvm::DIFile *file = _subprogram->getFile();
    const unsigned line = decl.token_varname.line();

    // **a name this compiler minted rather than lexed is artificial**, not hidden. `$__it` is exactly
    // what you want to look at when a `foreach` misbehaves, and DIFlagArtificial is what keeps it out
    // of `frame variable` until somebody asks - where dropping the declaration outright would be the
    // debugger seeing storage the program has.
    //
    // the token says whether this compiler minted it. a stdlib `$this` seen from a user unit has a
    // file and is not minted - those used to be indistinguishable when "no file" meant both
    const bool synthesized = decl.token_varname.is_minted();

    llvm::DINode::DIFlags flags =
        synthesized ? llvm::DINode::FlagArtificial : llvm::DINode::FlagZero;

    llvm::DILocalVariable *variable = arg_no.has_value()
        ? unit->builder->createParameterVariable(
            current_scope(), debugger_name(decl.name()), arg_no.value(), file, line, type,
            /*AlwaysPreserve=*/true, flags)
        : unit->builder->createAutoVariable(
            current_scope(), debugger_name(decl.name()), file, line, type,
            /*AlwaysPreserve=*/true, flags);

    // **at the alloca, not at the declaration's statement.** A slot lives in the entry block (see
    // CodegenContext::entry_alloca), so a record placed there dominates every use of it - where one
    // placed at the written position would not dominate a loop body's earlier turns
    unit->builder->insertDeclare(
        alloca,
        variable,
        unit->builder->createExpression(),
        llvm::DILocation::get(*_ctx.llvm_context, line, 0, current_scope()),
        alloca->getNextNode() != nullptr ? alloca->getNextNode()->getIterator()
                                         : alloca->getParent()->end());
}

llvm::DIType *DebugInfoCodegen::tuple_type_of(
    const AST::ValueType &type,
    CmpUnit &cmp_unit,
    const std::string &name,
    const std::vector<std::pair<std::string, llvm::DIType *>> &members,
    llvm::StructType *llvm_struct
)
{
    UnitDebug *unit = unit_debug(cmp_unit);
    const llvm::StructLayout *layout = _ctx.layout().getStructLayout(llvm_struct);

    std::vector<llvm::Metadata *> elements;

    for (size_t i = 0; i < members.size(); i++) {
        const auto &[member_name, member_type] = members[i];

        if (member_type == nullptr) {
            continue;
        }

        elements.push_back(unit->builder->createMemberType(
            unit->cu,
            member_name,
            /*File=*/nullptr,
            /*LineNumber=*/0,
            member_type->getSizeInBits(),
            member_type->getAlignInBits(),
            layout->getElementOffsetInBits(i),
            llvm::DINode::FlagZero,
            member_type));
    }

    // **no file at all**, unlike a declared struct: `int32?`, a callable and an interface value are
    // shapes the compiler mints rather than things anybody wrote, so naming a file would be naming
    // whichever unit happened to lower one first - see struct_type_of
    llvm::DICompositeType *composite = unit->builder->createStructType(
        unit->cu,
        name,
        /*File=*/nullptr,
        /*LineNumber=*/0,
        _ctx.layout().getTypeSizeInBits(llvm_struct),
        _ctx.layout().getABITypeAlign(llvm_struct).value() * 8,
        llvm::DINode::FlagZero,
        /*DerivedFrom=*/nullptr,
        unit->builder->getOrCreateArray(elements));

    unit->types[type] = composite;

    return composite;
}

llvm::DIType *DebugInfoCodegen::struct_type_of(const AST::ValueType &type, CmpUnit &cmp_unit)
{
    UnitDebug *unit = unit_debug(cmp_unit);
    const AST::ComplexType *complex = type.get_complex_type();

    llvm::Type *lowered = _ctx.types->get_llvm_type(type, cmp_unit);

    if (!lowered->isStructTy()) {
        return nullptr;
    }

    auto *llvm_struct = llvm::cast<llvm::StructType>(lowered);
    const std::string name = complex->namespaced_name();

    // **the file the type was declared in, never the unit's own.** A type description is emitted into
    // every unit that mentions it, so anything read from the ambient unit here makes two descriptions
    // of one type - which is what CodegenContext::type_site_map exists to prevent
    const auto site = _ctx.site_of(complex);
    llvm::DIFile *decl_file = site.has_value() ? file_for(*unit, site->file) : nullptr;
    const unsigned decl_line = site.has_value() ? site->line : 0;

    // **interned as a placeholder before its members are built.** A struct holding a `ptr` back to its
    // own type recurses through here forever otherwise - an infinite recursion rather than a wrong
    // answer, so this is not an optimization
    llvm::DICompositeType *placeholder = unit->builder->createReplaceableCompositeType(
        llvm::dwarf::DW_TAG_structure_type,
        name,
        unit->cu,
        decl_file,
        decl_line);

    unit->types[type] = placeholder;

    std::vector<llvm::Metadata *> elements;

    append_property_members(
        *unit,
        complex,
        cmp_unit,
        decl_file,
        decl_line,
        *_ctx.layout().getStructLayout(llvm_struct),
        llvm_struct->getNumElements(),
        /*base_offset_bits=*/0,
        elements);

    // `identifier:` is what lets llvm::Linker fold two units' descriptions of one Echo type into one -
    // the debug-info analogue of the linkonce_odr the symbol itself carries
    llvm::DICompositeType *composite = unit->builder->createStructType(
        unit->cu,
        name,
        decl_file,
        decl_line,
        _ctx.layout().getTypeSizeInBits(llvm_struct),
        _ctx.layout().getABITypeAlign(llvm_struct).value() * 8,
        llvm::DINode::FlagZero,
        /*DerivedFrom=*/nullptr,
        unit->builder->getOrCreateArray(elements),
        /*RunTimeLang=*/0,
        /*VTableHolder=*/nullptr,
        complex->mangled_token());

    unit->builder->replaceTemporary(llvm::TempDIType(placeholder), composite);
    unit->types[type] = composite;

    return composite;
}

void DebugInfoCodegen::append_property_members(
    UnitDebug &unit,
    const AST::ComplexType *complex,
    CmpUnit &cmp_unit,
    llvm::DIFile *decl_file,
    unsigned decl_line,
    const llvm::StructLayout &layout,
    size_t element_count,
    uint64_t base_offset_bits,
    std::vector<llvm::Metadata *> &elements
)
{
    // a 1:1 layout: the LLVM element index *is* Property::index. a packed enum overlays its
    // payload fields, so the offset comes from the structure table rather than the LLVM field
    const Structure *structure = nullptr;
    if (auto struct_id = cmp_unit.structure_table->get_structure_id(complex); struct_id != 0) {
        structure = &cmp_unit.structure_table->get_structure(struct_id);
    }

    const size_t count = complex->property_count();
    const bool packed = structure != nullptr && structure->has_packed_payload();

    for (size_t i = 0; i < count; i++) {
        if (!packed && i >= element_count) {
            break;
        }

        const AST::ComplexType::Property &property = complex->get_property(i);
        llvm::DIType *member_type = type_of(property.type, cmp_unit);

        if (member_type == nullptr) {
            continue;
        }

        const uint64_t offset_bits = packed
            ? structure->property_byte_offset[i] * 8
            : layout.getElementOffsetInBits(i);

        elements.push_back(unit.builder->createMemberType(
            unit.cu,
            property.name,
            decl_file,
            decl_line,
            member_type->getSizeInBits(),
            member_type->getAlignInBits(),
            base_offset_bits + offset_bits,
            property.is_private() ? llvm::DINode::FlagPrivate : llvm::DINode::FlagZero,
            member_type));
    }
}

llvm::DIType *DebugInfoCodegen::class_type_of(const AST::ValueType &type, CmpUnit &cmp_unit)
{
    UnitDebug *unit = unit_debug(cmp_unit);
    const AST::ComplexType *complex = type.get_complex_type();

    if (complex == nullptr) {
        return nullptr;
    }

    ClassLayout class_layout = _ctx.types->get_or_create_class_layout(complex, cmp_unit);

    if (class_layout.box == nullptr || class_layout.payload == nullptr) {
        return nullptr;
    }

    const std::string name = complex->namespaced_name();

    // the declaring file, for struct_type_of's reason
    const auto site = _ctx.site_of(complex);
    llvm::DIFile *decl_file = site.has_value() ? file_for(*unit, site->file) : nullptr;
    const unsigned decl_line = site.has_value() ? site->line : 0;

    // a class *value* is a handle into the heap block, so what a debugger must be told is "pointer to
    // the box". Interned as a placeholder first for struct_type_of's reason - a class naming itself is
    // the ordinary case rather than the exotic one
    llvm::DICompositeType *placeholder = unit->builder->createReplaceableCompositeType(
        llvm::dwarf::DW_TAG_structure_type, name, unit->cu, decl_file, decl_line);

    llvm::DIType *box_placeholder_ptr = unit->builder->createPointerType(
        placeholder, ECO_TARGET_POINTER_SIZE * 8);

    unit->types[type] = box_placeholder_ptr;

    const llvm::StructLayout *box_layout = _ctx.layout().getStructLayout(class_layout.box);
    std::vector<llvm::Metadata *> elements;

    // the three header words, marked artificial: they are the reference-counting machinery rather than
    // anything the author declared, so `frame variable` keeps them out of the way while `p` can still
    // reach them when a leak is what is being chased
    llvm::DIType *counter = unit->builder->createBasicType(
        "usize", 64, llvm::dwarf::DW_ATE_unsigned);

    const std::array<std::pair<const char *, llvm::DIType *>, 3> header = {{
        { "__strong", counter },
        { "__weak", counter },
        { "__typeinfo", opaque_pointer(*unit->builder) },
    }};

    for (size_t i = 0; i < header.size(); i++) {
        elements.push_back(unit->builder->createMemberType(
            unit->cu, header[i].first, decl_file, decl_line,
            header[i].second->getSizeInBits(), header[i].second->getAlignInBits(),
            box_layout->getElementOffsetInBits(i),
            llvm::DINode::FlagArtificial, header[i].second));
    }

    // **the payload's properties are hoisted into the box at their box offsets**, rather than nested
    // under a `__payload` member. Both describe the same bytes; this one makes `p $obj->x` work, which
    // is what a person actually types
    append_property_members(
        *unit,
        complex,
        cmp_unit,
        decl_file,
        decl_line,
        *_ctx.layout().getStructLayout(class_layout.payload),
        class_layout.payload->getNumElements(),
        box_layout->getElementOffsetInBits(ClassBox::payload_index),
        elements);

    llvm::DICompositeType *box = unit->builder->createStructType(
        unit->cu,
        // **named after the class, not `<Name>.box`.** The box is how a class is stored rather than a
        // second type, so a variable reads `(Thing *) t` the way a person wrote it. The `identifier:`
        // below still carries the `.box` suffix and is what actually uniques it across units, so
        // nothing depends on this name being distinct from the payload's
        name,
        decl_file,
        decl_line,
        _ctx.layout().getTypeSizeInBits(class_layout.box),
        _ctx.layout().getABITypeAlign(class_layout.box).value() * 8,
        llvm::DINode::FlagZero,
        /*DerivedFrom=*/nullptr,
        unit->builder->getOrCreateArray(elements),
        /*RunTimeLang=*/0,
        /*VTableHolder=*/nullptr,
        complex->mangled_token() + ".box");

    unit->builder->replaceTemporary(llvm::TempDIType(placeholder), box);

    llvm::DIType *handle = unit->builder->createPointerType(box, ECO_TARGET_POINTER_SIZE * 8);
    unit->types[type] = handle;

    return handle;
}

llvm::DIType *DebugInfoCodegen::type_of(const AST::ValueType &type, CmpUnit &cmp_unit)
{
    UnitDebug *unit = unit_debug(cmp_unit);

    if (unit == nullptr) {
        return nullptr;
    }

    auto cached = unit->types.find(type);

    if (cached != unit->types.end()) {
        return cached->second;
    }

    // the arm order is get_llvm_type's, because it is the same taxonomy: a wrapped optional first, then
    // everything whose machine value is one address, then the aggregates

    if (type.is_wrapped_optional()) {
        llvm::StructType *wrapper = _ctx.types->optional_llvm_type(type, cmp_unit);

        llvm::DIType *has = unit->builder->createBasicType(
            "__has", 8, llvm::dwarf::DW_ATE_boolean);

        return tuple_type_of(
            type,
            cmp_unit,
            // **the type's own spelling, not the constant "optional".** Every wrapped optional shared
            // one name, so `int32?` and `string?` were one type as far as a debugger was concerned -
            // information lost in the object rather than a presentation problem, and unmatchable by a
            // formatter. get_type_desciption is the same renderer namespaced_name() uses for an
            // instantiation's arguments, so the two spell a type the same way
            type.get_type_desciption(),
            { { "__has", has },
              { "__value", type_of(AST::ValueType::make_non_nullable(type), cmp_unit) } },
            wrapper);
    }

    if (type.is_pointer() || type.is_weak()) {
        // **a pointer does drag its pointee in here, where get_llvm_type deliberately does not.** That
        // one answers a bare `ptr` so a borrow parameter never forces a layout into a unit that has not
        // declared it; a DIDerivedType costs nothing at that level, and pointing at nothing is the
        // difference between `p *$node` working and not
        llvm::DIType *pointee = type_of(AST::value_type_of(type), cmp_unit);
        llvm::DIType *result =
            unit->builder->createPointerType(pointee, ECO_TARGET_POINTER_SIZE * 8);

        unit->types[type] = result;
        return result;
    }

    if (type.is_class()) {
        return class_type_of(type, cmp_unit);
    }

    if (type.is_c_function()) {
        // a pointer to a subroutine type - the correct DWARF, and lldb formats it for free. the
        // callable beside it is a synthetic `{ __fn, __env }` because that *is* two words
        std::vector<llvm::Metadata *> signature;
        signature.push_back(type_of(type.signature().return_type, cmp_unit));

        for (const auto &parameter : type.signature().parameter_types) {
            signature.push_back(type_of(parameter, cmp_unit));
        }

        llvm::DISubroutineType *subroutine =
            unit->builder->createSubroutineType(unit->builder->getOrCreateTypeArray(signature));
        llvm::DIType *result =
            unit->builder->createPointerType(subroutine, ECO_TARGET_POINTER_SIZE * 8);

        unit->types[type] = result;
        return result;
    }

    // the two shapes that are a pair of addresses and nothing else. one call site, because they differ
    // only in what the two halves are called
    if (type.is_callable() || type.is_interface()) {
        llvm::DIType *opaque = opaque_pointer(*unit->builder);

        const bool callable = type.is_callable();

        return tuple_type_of(
            type,
            cmp_unit,
            type.get_type_desciption(),
            { { callable ? "__fn" : "__object", opaque },
              { callable ? "__env" : "__vtable", opaque } },
            callable ? _ctx.types->callable_llvm_type() : _ctx.types->iface_llvm_type());
    }

    if (type.is_primitive()) {
        // **a `bool` is 8 bits here, not 1** - an `i1` is how it is computed and a byte is how it is
        // stored, and a debugger reads storage. get_primitive_size already answers in stored bytes
        const unsigned bits = AST::get_primitive_size(type.get_primitive_type()) * 8;
        const unsigned encoding = basic_type_encoding(type);

        // `void`, and anything else with no storage: a null DIType is how DWARF spells that, and it is
        // legal in every position one can appear
        if (bits == 0 || encoding == 0) {
            return nullptr;
        }

        llvm::DIType *result = unit->builder->createBasicType(
            AST::get_primitive_name(type.get_primitive_type()), bits, encoding);

        unit->types[type] = result;
        return result;
    }

    // an enum describes as the aggregate it is - `__tag` and the payload slots, by their property
    // names. deliberately the struct path rather than a DW_TAG_variant_part: what DWARF would gain is
    // a debugger showing only the live case, and what it costs is a second description of a layout
    // this one already gets right. tools/echo_lldb.py is where that presentation belongs
    if (type.is_struct() || type.is_enum()) {
        return struct_type_of(type, cmp_unit);
    }

    // a generic parameter that reached codegen unbound, or a kind added later. not inspectable, and
    // deliberately not an error: debug info may never be the thing that fails a build
    return nullptr;
}

void DebugInfoCodegen::finalize_all()
{
    if (!enabled()) {
        return;
    }

    for (auto &[cmp_unit, unit] : _units) {
        if (unit.builder == nullptr) {
            continue;
        }

        unit.builder->finalize();
    }

#ifndef NDEBUG
    // **LLVMCompiler::compile_bundle verifies the main module only**, so a malformed scope chain in a
    // library unit would otherwise reach the object writer unchallenged. LLVM's debug-info verifier is
    // the most complete check available for any of this, and pointing it at every unit costs a debug
    // build one walk
    for (auto &[cmp_unit, unit] : _units) {
        if (cmp_unit->llvm_module == nullptr) {
            continue;
        }

        std::string error;
        llvm::raw_string_ostream stream(error);

        if (llvm::verifyModule(*cmp_unit->llvm_module, &stream)) {
            throw Compiler::InternalCompilerException(fmt::format(
                "debug info verification failed for module '{}':\n{}",
                cmp_unit->ast_module->name, error));
        }
    }
#endif
}

};
