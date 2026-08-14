#include "Compiler/LLVM/OdrComparison.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Comdat.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cctype>
#include <map>
#include <set>
#include <utility>

namespace
{

// `array<int32>.2` -> `array<int32>`. Every unit mints its own `llvm::StructType` for one Echo type
// and they all share a context, so the second one created is renamed - a fact about the order the
// units were lowered in, never about the type. IRMover unifies them structurally when the modules
// are merged, and under separate object files the name is gone entirely; only the layout survives
std::string strip_uniquing_suffix(llvm::StringRef name)
{
    size_t end = name.size();

    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1]))) {
        end--;
    }

    const bool is_suffix = end > 1 && end < name.size() && name[end - 1] == '.';

    return is_suffix ? name.substr(0, end - 1).str() : name.str();
}

// the lock-step walk, holding the two numberings and the three "already being compared" sets between
// them. one instance per pair of functions, so nothing survives a comparison
class Comparison
{
public:
    std::optional<Compiler::LLVM::OdrDifference> run(const llvm::Function &left, const llvm::Function &right);

private:
    // each of these answers *what* diverged, or null for "these agree". a string rather than a bool
    // so the exception can point at a thing instead of at two rendered bodies
    const char *compare_signature(const llvm::Function &left, const llvm::Function &right);
    const char *compare_instruction(const llvm::Instruction &left, const llvm::Instruction &right);
    const char *compare_operation(const llvm::Instruction &left, const llvm::Instruction &right);
    const char *compare_attached_metadata(const llvm::Value &left, const llvm::Value &right);
    const char *compare_debug_records(const llvm::Instruction &left, const llvm::Instruction &right);

    bool same_type(llvm::Type *left, llvm::Type *right);
    bool same_type_shape(llvm::Type *left, llvm::Type *right);
    bool same_value(const llvm::Value *left, const llvm::Value *right);
    bool same_constant(const llvm::Constant *left, const llvm::Constant *right);
    bool same_global(const llvm::GlobalValue *left, const llvm::GlobalValue *right);
    bool same_global_content(const llvm::GlobalValue *left, const llvm::GlobalValue *right);
    bool same_metadata(const llvm::Metadata *left, const llvm::Metadata *right);
    bool same_metadata_content(const llvm::Metadata *left, const llvm::Metadata *right);
    static bool same_debug_fields(const llvm::MDNode *left, const llvm::MDNode *right);

    typedef llvm::DenseMap<const llvm::Value *, unsigned> Serials;
    typedef std::pair<const void *, const void *> Pairing;

    // the position a value holds in its own function, which is the only thing about it that means
    // anything on the other side of a module boundary
    static void number_body(const llvm::Function &fn, Serials &serials);
    static unsigned serial_of(const llvm::Value *value, Serials &serials);

    // **three graphs that can reach themselves** - a type through a member, a global through its own
    // initializer, a debug node through the scope chain that names it back. so the walk carries a
    // memo *plus* a stack: a pair still being compared further up is assumed to agree and whatever
    // asked is what decides, and the verdict is recorded once it exists.
    //
    // one stack for all three, for the reason the one memo below already rests on: a key is two
    // pointers into two modules and never two of the same kind, so the three keyspaces cannot meet.
    // three sets threaded through as a parameter would be three spellings of one question.
    //
    // the memo is not an optimization here so much as the thing that makes the walk affordable at
    // all - one pair of named struct types is asked about once per operand of every instruction in
    // every shared body, and answering it afresh each time was the whole cost of this check
    bool decided(const Pairing &key, bool &answer) const;
    bool enter(const Pairing &key);
    bool leave(const Pairing &key, bool answer);

    std::map<Pairing, bool> _verdicts;
    std::set<Pairing> _in_progress;

    Serials _left_serials;
    Serials _right_serials;
};

// a pair whose verdict is already known. the three questions share one memo, the key being two
// pointers into two modules and never two of the same kind
bool Comparison::decided(const Pairing &key, bool &answer) const
{
    auto found = _verdicts.find(key);

    if (found == _verdicts.end()) {
        return false;
    }

    answer = found->second;

    return true;
}

// false when the pair is already on the stack, which is where a cycle stops
bool Comparison::enter(const Pairing &key)
{
    return _in_progress.insert(key).second;
}

bool Comparison::leave(const Pairing &key, bool answer)
{
    _in_progress.erase(key);
    _verdicts.insert({ key, answer });

    return answer;
}

void Comparison::number_body(const llvm::Function &fn, Serials &serials)
{
    for (const llvm::Argument &argument : fn.args()) {
        serial_of(&argument, serials);
    }

    // blocks before instructions, and both in written order, so a forward branch and a phi's
    // predecessor already have a number by the time an operand asks for one
    for (const llvm::BasicBlock &block : fn) {
        serial_of(&block, serials);
    }

    for (const llvm::BasicBlock &block : fn) {
        for (const llvm::Instruction &inst : block) {
            serial_of(&inst, serials);
        }
    }
}

unsigned Comparison::serial_of(const llvm::Value *value, Serials &serials)
{
    auto found = serials.find(value);

    if (found != serials.end()) {
        return found->second;
    }

    const unsigned assigned = static_cast<unsigned>(serials.size());
    serials.insert({ value, assigned });

    return assigned;
}

bool Comparison::same_type(llvm::Type *left, llvm::Type *right)
{
    // types are context-owned, so identity is the answer almost every time and everything below is
    // reached only for a named struct two units both minted
    if (left == right) {
        return true;
    }

    if (left == nullptr || right == nullptr || left->getTypeID() != right->getTypeID()) {
        return false;
    }

    const Pairing key{ left, right };
    bool remembered = false;

    if (decided(key, remembered)) {
        return remembered;
    }

    if (!enter(key)) {
        return true;
    }

    return leave(key, same_type_shape(left, right));
}

bool Comparison::same_type_shape(llvm::Type *left, llvm::Type *right)
{
    switch (left->getTypeID()) {
        case llvm::Type::IntegerTyID:
            return left->getIntegerBitWidth() == right->getIntegerBitWidth();

        case llvm::Type::PointerTyID:
            return left->getPointerAddressSpace() == right->getPointerAddressSpace();

        case llvm::Type::ArrayTyID:
            return left->getArrayNumElements() == right->getArrayNumElements()
                && same_type(left->getArrayElementType(), right->getArrayElementType());

        case llvm::Type::FixedVectorTyID:
        case llvm::Type::ScalableVectorTyID: {
            auto *left_vector = llvm::cast<llvm::VectorType>(left);
            auto *right_vector = llvm::cast<llvm::VectorType>(right);

            return left_vector->getElementCount() == right_vector->getElementCount()
                && same_type(left_vector->getElementType(), right_vector->getElementType());
        }

        case llvm::Type::FunctionTyID: {
            auto *left_fn = llvm::cast<llvm::FunctionType>(left);
            auto *right_fn = llvm::cast<llvm::FunctionType>(right);

            if (left_fn->isVarArg() != right_fn->isVarArg()
                || left_fn->getNumParams() != right_fn->getNumParams()) {
                return false;
            }

            if (!same_type(left_fn->getReturnType(), right_fn->getReturnType())) {
                return false;
            }

            for (unsigned i = 0; i < left_fn->getNumParams(); i++) {
                if (!same_type(left_fn->getParamType(i), right_fn->getParamType(i))) {
                    return false;
                }
            }

            return true;
        }

        case llvm::Type::StructTyID: {
            auto *left_struct = llvm::cast<llvm::StructType>(left);
            auto *right_struct = llvm::cast<llvm::StructType>(right);

            if (left_struct->isPacked() != right_struct->isPacked()
                || left_struct->isOpaque() != right_struct->isOpaque()
                || left_struct->hasName() != right_struct->hasName()) {
                return false;
            }

            // the name is compared and the suffix is not - a layout alone would let two unrelated
            // Echo types of one shape pass for each other, which is weaker than what this watches
            if (left_struct->hasName()
                && strip_uniquing_suffix(left_struct->getName())
                    != strip_uniquing_suffix(right_struct->getName())) {
                return false;
            }

            if (left_struct->isOpaque()) {
                return true;
            }

            if (left_struct->getNumElements() != right_struct->getNumElements()) {
                return false;
            }

            for (unsigned i = 0; i < left_struct->getNumElements(); i++) {
                if (!same_type(left_struct->getElementType(i), right_struct->getElementType(i))) {
                    return false;
                }
            }

            return true;
        }

        default:
            // void, label, metadata, token and every floating point width: a kind carrying nothing
            // the id above has not already answered
            return true;
    }
}

bool Comparison::same_global(const llvm::GlobalValue *left, const llvm::GlobalValue *right)
{
    if (left == right) {
        return true;
    }

    const Pairing key{ left, right };
    bool remembered = false;

    if (decided(key, remembered)) {
        return remembered;
    }

    if (!enter(key)) {
        return true;
    }

    return leave(key, same_global_content(left, right));
}

bool Comparison::same_global_content(const llvm::GlobalValue *left, const llvm::GlobalValue *right)
{
    if (left->getValueID() != right->getValueID() || left->getLinkage() != right->getLinkage()) {
        return false;
    }

    if (!same_type(left->getValueType(), right->getValueType())) {
        return false;
    }

    // **a module-local name says nothing**, being a slot the printer numbered rather than anything the
    // compiler chose - so two bodies reaching two *different* private constants are indistinguishable
    // by name. Which is exactly the divergence this check exists for: an abort message is a private
    // string built from whichever file the compiler happened to be walking. the content is what agrees
    if (left->hasLocalLinkage()) {
        auto *left_variable = llvm::dyn_cast<llvm::GlobalVariable>(left);
        auto *right_variable = llvm::dyn_cast<llvm::GlobalVariable>(right);

        // a private *function* has nothing else to go on, and nothing here emits one
        if (left_variable == nullptr || right_variable == nullptr) {
            return left->getName() == right->getName();
        }

        if (left_variable->isConstant() != right_variable->isConstant()
            || left_variable->hasInitializer() != right_variable->hasInitializer()) {
            return false;
        }

        if (!left_variable->hasInitializer()) {
            return true;
        }

        return same_constant(left_variable->getInitializer(), right_variable->getInitializer());
    }

    // and a linked name is the whole of what the linker will match on
    return left->getName() == right->getName();
}

bool Comparison::same_constant(const llvm::Constant *left, const llvm::Constant *right)
{
    // an integer, a float, a null, a zeroinitializer and every aggregate built only out of those are
    // uniqued in the context, so they are one object however many modules mention them
    if (left == right) {
        return true;
    }

    if (left->getValueID() != right->getValueID() || !same_type(left->getType(), right->getType())) {
        return false;
    }

    if (auto *global = llvm::dyn_cast<llvm::GlobalValue>(left)) {
        return same_global(global, llvm::cast<llvm::GlobalValue>(right));
    }

    if (auto *integer = llvm::dyn_cast<llvm::ConstantInt>(left)) {
        return integer->getValue() == llvm::cast<llvm::ConstantInt>(right)->getValue();
    }

    if (auto *floating = llvm::dyn_cast<llvm::ConstantFP>(left)) {
        return floating->getValueAPF().bitwiseIsEqual(
            llvm::cast<llvm::ConstantFP>(right)->getValueAPF());
    }

    // a string literal's bytes, which are the constant rather than an operand list
    if (auto *data = llvm::dyn_cast<llvm::ConstantDataSequential>(left)) {
        return data->getRawDataValues() == llvm::cast<llvm::ConstantDataSequential>(right)->getRawDataValues();
    }

    if (auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(left)) {
        auto *other = llvm::cast<llvm::ConstantExpr>(right);

        if (expression->getOpcode() != other->getOpcode()) {
            return false;
        }

        if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(expression)) {
            if (!same_type(gep->getSourceElementType(),
                    llvm::cast<llvm::GEPOperator>(other)->getSourceElementType())) {
                return false;
            }
        }
    }

    // what is left is a constant whose operands are its content - an aggregate, a constant
    // expression, a `blockaddress`, or one of the wrappers around a global - and one whose type was
    // the whole of it, which has no operands and has already answered
    if (left->getNumOperands() != right->getNumOperands()) {
        return false;
    }

    for (unsigned i = 0; i < left->getNumOperands(); i++) {
        if (!same_value(left->getOperand(i), right->getOperand(i))) {
            return false;
        }
    }

    return true;
}

bool Comparison::same_value(const llvm::Value *left, const llvm::Value *right)
{
    if (llvm::isa<llvm::Constant>(left) || llvm::isa<llvm::Constant>(right)) {
        if (!llvm::isa<llvm::Constant>(left) || !llvm::isa<llvm::Constant>(right)) {
            return false;
        }

        return same_constant(llvm::cast<llvm::Constant>(left), llvm::cast<llvm::Constant>(right));
    }

    // `asm` has neither a symbol nor storage; its text is the whole of it
    if (llvm::isa<llvm::InlineAsm>(left) || llvm::isa<llvm::InlineAsm>(right)) {
        auto *left_asm = llvm::dyn_cast<llvm::InlineAsm>(left);
        auto *right_asm = llvm::dyn_cast<llvm::InlineAsm>(right);

        return left_asm != nullptr && right_asm != nullptr
            && left_asm->getAsmString() == right_asm->getAsmString()
            && left_asm->getConstraintString() == right_asm->getConstraintString()
            && same_type(left_asm->getFunctionType(), right_asm->getFunctionType());
    }

    if (llvm::isa<llvm::MetadataAsValue>(left) || llvm::isa<llvm::MetadataAsValue>(right)) {
        auto *left_meta = llvm::dyn_cast<llvm::MetadataAsValue>(left);
        auto *right_meta = llvm::dyn_cast<llvm::MetadataAsValue>(right);

        return left_meta != nullptr && right_meta != nullptr
            && same_metadata(left_meta->getMetadata(), right_meta->getMetadata());
    }

    // an argument, a block or an instruction: a *position* in the body, and both walks numbered them
    // in the same order, so the numbers are what has to agree
    if (!same_type(left->getType(), right->getType())) {
        return false;
    }

    return serial_of(left, _left_serials) == serial_of(right, _right_serials);
}

// **what a debug node keeps outside its operand list.** A `DILocation`'s line and column, a
// `DISubprogram`'s scope line, a `DIBasicType`'s encoding: LLVM stores these as fields rather than as
// operands, so the operand walk below cannot see one - and a line taken from the ambient walk rather than
// from the declaration is exactly the divergence this check exists for.
//
// **identity does not answer it either**, although a uniqued node's identity is its whole content: two
// units reach two different `DISubprogram` *definitions*, which are distinct nodes, so every uniqued node
// above them is a different pointer as well however identical its fields.
//
// asked through the base classes wherever there is one, so a `DIType` LLVM adds later arrives with its
// line, size, alignment, offset and flags already compared and only its own leaf field left. What is left
// after that is a real hole rather than a covered one, and it is the reason the arms below are grouped
// by base rather than written one per kind
bool Comparison::same_debug_fields(const llvm::MDNode *left, const llvm::MDNode *right)
{
    if (auto *node = llvm::dyn_cast<llvm::DINode>(left)) {
        if (node->getTag() != llvm::cast<llvm::DINode>(right)->getTag()) {
            return false;
        }
    }

    // every DIBasicType, DIDerivedType, DICompositeType, DISubroutineType and DIStringType at once
    if (auto *type = llvm::dyn_cast<llvm::DIType>(left)) {
        auto *other = llvm::cast<llvm::DIType>(right);

        if (type->getLine() != other->getLine() || type->getSizeInBits() != other->getSizeInBits()
            || type->getAlignInBits() != other->getAlignInBits()
            || type->getOffsetInBits() != other->getOffsetInBits()
            || type->getFlags() != other->getFlags()) {
            return false;
        }
    }

    // and every DILocalVariable and DIGlobalVariable
    if (auto *variable = llvm::dyn_cast<llvm::DIVariable>(left)) {
        auto *other = llvm::cast<llvm::DIVariable>(right);

        if (variable->getLine() != other->getLine()
            || variable->getAlignInBits() != other->getAlignInBits()) {
            return false;
        }
    }

    if (auto *basic = llvm::dyn_cast<llvm::DIBasicType>(left)) {
        return basic->getEncoding() == llvm::cast<llvm::DIBasicType>(right)->getEncoding();
    }

    if (auto *composite = llvm::dyn_cast<llvm::DICompositeType>(left)) {
        return composite->getRuntimeLang() == llvm::cast<llvm::DICompositeType>(right)->getRuntimeLang();
    }

    if (auto *signature = llvm::dyn_cast<llvm::DISubroutineType>(left)) {
        return signature->getCC() == llvm::cast<llvm::DISubroutineType>(right)->getCC();
    }

    if (auto *subprogram = llvm::dyn_cast<llvm::DISubprogram>(left)) {
        auto *other = llvm::cast<llvm::DISubprogram>(right);

        return subprogram->getLine() == other->getLine()
            && subprogram->getScopeLine() == other->getScopeLine()
            && subprogram->getFlags() == other->getFlags()
            && subprogram->getSPFlags() == other->getSPFlags()
            && subprogram->getVirtualIndex() == other->getVirtualIndex()
            && subprogram->getThisAdjustment() == other->getThisAdjustment();
    }

    if (auto *local = llvm::dyn_cast<llvm::DILocalVariable>(left)) {
        auto *other = llvm::cast<llvm::DILocalVariable>(right);

        return local->getArg() == other->getArg() && local->getFlags() == other->getFlags();
    }

    if (auto *location = llvm::dyn_cast<llvm::DILocation>(left)) {
        auto *other = llvm::cast<llvm::DILocation>(right);

        return location->getLine() == other->getLine() && location->getColumn() == other->getColumn()
            && location->isImplicitCode() == other->isImplicitCode();
    }

    if (auto *block = llvm::dyn_cast<llvm::DILexicalBlock>(left)) {
        auto *other = llvm::cast<llvm::DILexicalBlock>(right);

        return block->getLine() == other->getLine() && block->getColumn() == other->getColumn();
    }

    if (auto *block_file = llvm::dyn_cast<llvm::DILexicalBlockFile>(left)) {
        return block_file->getDiscriminator()
            == llvm::cast<llvm::DILexicalBlockFile>(right)->getDiscriminator();
    }

    // the one whose whole content is a field: `DW_OP_deref` and the rest are integers on the node
    if (auto *expression = llvm::dyn_cast<llvm::DIExpression>(left)) {
        return expression->getElements() == llvm::cast<llvm::DIExpression>(right)->getElements();
    }

    if (auto *file = llvm::dyn_cast<llvm::DIFile>(left)) {
        auto *other = llvm::cast<llvm::DIFile>(right);

        return file->getChecksum() == other->getChecksum()
            && file->getSource() == other->getSource();
    }

    if (auto *range = llvm::dyn_cast<llvm::DISubrange>(left)) {
        (void)range;
        // count, lower bound, upper bound and stride are all operands
        return true;
    }

    return true;
}

bool Comparison::same_metadata(const llvm::Metadata *left, const llvm::Metadata *right)
{
    // uniqued in the shared context, so identical content is one node however many modules reach it.
    // that is the answer for every `!tbaa` leaf, for a `DILocation`, for a `DIFile` and for a
    // composite type reached through its ODR identifier - what falls through below is the *distinct*
    // nodes, of which a body carries exactly one kind
    if (left == right) {
        return true;
    }

    if (left == nullptr || right == nullptr || left->getMetadataID() != right->getMetadataID()) {
        return false;
    }

    const Pairing key{ left, right };
    bool remembered = false;

    if (decided(key, remembered)) {
        return remembered;
    }

    // the debug graph is cyclic: a subprogram names its unit, and the unit's subprogram list names it
    // back. a pair already on the stack is taken as agreeing and the question that asked decides
    if (!enter(key)) {
        return true;
    }

    return leave(key, same_metadata_content(left, right));
}

bool Comparison::same_metadata_content(const llvm::Metadata *left, const llvm::Metadata *right)
{
    if (auto *text = llvm::dyn_cast<llvm::MDString>(left)) {
        return text->getString() == llvm::cast<llvm::MDString>(right)->getString();
    }

    // a local wrapped as metadata - the alloca a `#dbg_declare` describes - so this is the same
    // position question an operand asks
    if (auto *wrapped = llvm::dyn_cast<llvm::ValueAsMetadata>(left)) {
        return same_value(wrapped->getValue(), llvm::cast<llvm::ValueAsMetadata>(right)->getValue());
    }

    auto *left_node = llvm::dyn_cast<llvm::MDNode>(left);
    auto *right_node = llvm::dyn_cast<llvm::MDNode>(right);

    if (left_node == nullptr || right_node == nullptr) {
        return false;
    }

    // **the compile unit is a fact about the module, not about the body.** every scope chain ends at
    // one and it names the module's first file, which is *supposed* to differ between two units - so
    // the walk stops here rather than comparing it. the types beneath it are deduplicated by the
    // linker on their `identifier:`, exactly as C++ does across translation units
    if (llvm::isa<llvm::DICompileUnit>(left_node)) {
        return true;
    }

    if (!same_debug_fields(left_node, right_node)) {
        return false;
    }

    if (left_node->getNumOperands() != right_node->getNumOperands()) {
        return false;
    }

    for (unsigned i = 0; i < left_node->getNumOperands(); i++) {
        if (!same_metadata(left_node->getOperand(i).get(), right_node->getOperand(i).get())) {
            return false;
        }
    }

    return true;
}

const char *Comparison::compare_attached_metadata(const llvm::Value &left, const llvm::Value &right)
{
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> left_attached;
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> right_attached;

    // every kind, not just `!dbg`. `!tbaa` is the other one a body carries today, and singling debug
    // info out would leave exactly the hole this covers - one that opens the day an access family
    // stops being a pure function of the declaration. getAllMetadata is ordered by kind id, so the
    // walk is deterministic without sorting
    if (auto *inst = llvm::dyn_cast<llvm::Instruction>(&left)) {
        inst->getAllMetadata(left_attached);
        llvm::cast<llvm::Instruction>(&right)->getAllMetadata(right_attached);
    } else {
        llvm::cast<llvm::Function>(&left)->getAllMetadata(left_attached);
        llvm::cast<llvm::Function>(&right)->getAllMetadata(right_attached);
    }

    // carrying none is as much a fact about a body as carrying some: the prologue and the emitted
    // runtime deliberately have no position, and a copy that grew one is what this is watching for
    if (left_attached.size() != right_attached.size()) {
        return "metadata attachments";
    }

    for (size_t i = 0; i < left_attached.size(); i++) {
        if (left_attached[i].first != right_attached[i].first) {
            return "metadata kind";
        }

        if (!same_metadata(left_attached[i].second, right_attached[i].second)) {
            return "metadata";
        }
    }

    return nullptr;
}

const char *Comparison::compare_debug_records(const llvm::Instruction &left, const llvm::Instruction &right)
{
    // **a `#dbg_declare` is not an instruction any more.** since LLVM 19 a variable's description
    // hangs off the instruction it precedes rather than being a call in the block, so the walk above
    // cannot see one - and what a local is called, and where, is squarely inside the promise that two
    // copies of one body are identical
    auto left_record = left.getDbgRecordRange().begin();
    auto right_record = right.getDbgRecordRange().begin();

    const auto left_end = left.getDbgRecordRange().end();
    const auto right_end = right.getDbgRecordRange().end();

    for (; left_record != left_end && right_record != right_end; ++left_record, ++right_record) {
        if (left_record->getRecordKind() != right_record->getRecordKind()) {
            return "debug record kind";
        }

        if (!same_metadata(
                left_record->getDebugLoc().getAsMDNode(), right_record->getDebugLoc().getAsMDNode())) {
            return "debug record location";
        }

        if (auto *variable = llvm::dyn_cast<llvm::DbgVariableRecord>(&*left_record)) {
            auto *other = llvm::cast<llvm::DbgVariableRecord>(&*right_record);

            if (variable->getType() != other->getType()) {
                return "debug record kind";
            }

            if (!same_metadata(variable->getRawVariable(), other->getRawVariable())) {
                return "described variable";
            }

            if (!same_metadata(variable->getRawExpression(), other->getRawExpression())) {
                return "debug expression";
            }

            if (!same_metadata(variable->getRawLocation(), other->getRawLocation())) {
                return "described storage";
            }
        } else if (auto *label = llvm::dyn_cast<llvm::DbgLabelRecord>(&*left_record)) {
            if (!same_metadata(
                    label->getRawLabel(), llvm::cast<llvm::DbgLabelRecord>(&*right_record)->getRawLabel())) {
                return "debug label";
            }
        }
    }

    if (left_record != left_end || right_record != right_end) {
        return "debug record count";
    }

    return nullptr;
}

const char *Comparison::compare_operation(const llvm::Instruction &left, const llvm::Instruction &right)
{
    // what an opcode carries beside its operands. the arms are the ones LLVM keeps outside the
    // operand list; the flag word below covers nuw/nsw/exact, the GEP's nowrap bits and fast-math
    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&left)) {
        auto *other = llvm::cast<llvm::AllocaInst>(&right);

        if (!same_type(alloca->getAllocatedType(), other->getAllocatedType())) {
            return "allocated type";
        }

        return alloca->getAlign() == other->getAlign() ? nullptr : "alloca alignment";
    }

    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&left)) {
        auto *other = llvm::cast<llvm::LoadInst>(&right);

        if (load->isVolatile() != other->isVolatile() || load->getAlign() != other->getAlign()) {
            return "load shape";
        }

        return load->getOrdering() == other->getOrdering()
                && load->getSyncScopeID() == other->getSyncScopeID()
            ? nullptr
            : "load atomicity";
    }

    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&left)) {
        auto *other = llvm::cast<llvm::StoreInst>(&right);

        if (store->isVolatile() != other->isVolatile() || store->getAlign() != other->getAlign()) {
            return "store shape";
        }

        return store->getOrdering() == other->getOrdering()
                && store->getSyncScopeID() == other->getSyncScopeID()
            ? nullptr
            : "store atomicity";
    }

    if (auto *comparison = llvm::dyn_cast<llvm::CmpInst>(&left)) {
        return comparison->getPredicate() == llvm::cast<llvm::CmpInst>(&right)->getPredicate()
            ? nullptr
            : "comparison predicate";
    }

    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&left)) {
        return same_type(gep->getSourceElementType(),
                   llvm::cast<llvm::GetElementPtrInst>(&right)->getSourceElementType())
            ? nullptr
            : "indexed type";
    }

    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&left)) {
        auto *other = llvm::cast<llvm::CallBase>(&right);

        if (!same_type(call->getFunctionType(), other->getFunctionType())) {
            return "call signature";
        }

        if (call->getCallingConv() != other->getCallingConv()) {
            return "call convention";
        }

        // uniqued in the shared context, so this is exact rather than a rendering of it
        if (call->getAttributes() != other->getAttributes()) {
            return "call attributes";
        }

        if (auto *direct = llvm::dyn_cast<llvm::CallInst>(call)) {
            if (direct->getTailCallKind() != llvm::cast<llvm::CallInst>(other)->getTailCallKind()) {
                return "tail call kind";
            }
        }

        return nullptr;
    }

    if (auto *insert = llvm::dyn_cast<llvm::InsertValueInst>(&left)) {
        return insert->getIndices() == llvm::cast<llvm::InsertValueInst>(&right)->getIndices()
            ? nullptr
            : "inserted slot";
    }

    if (auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(&left)) {
        return extract->getIndices() == llvm::cast<llvm::ExtractValueInst>(&right)->getIndices()
            ? nullptr
            : "extracted slot";
    }

    // a phi's predecessors are kept beside its operands rather than among them
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&left)) {
        auto *other = llvm::cast<llvm::PHINode>(&right);

        if (phi->getNumIncomingValues() != other->getNumIncomingValues()) {
            return "phi arity";
        }

        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
            if (!same_value(phi->getIncomingBlock(i), other->getIncomingBlock(i))) {
                return "phi predecessor";
            }
        }

        return nullptr;
    }

    if (auto *fence = llvm::dyn_cast<llvm::FenceInst>(&left)) {
        auto *other = llvm::cast<llvm::FenceInst>(&right);

        return fence->getOrdering() == other->getOrdering()
                && fence->getSyncScopeID() == other->getSyncScopeID()
            ? nullptr
            : "fence atomicity";
    }

    if (auto *exchange = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&left)) {
        auto *other = llvm::cast<llvm::AtomicCmpXchgInst>(&right);

        return exchange->isVolatile() == other->isVolatile()
                && exchange->isWeak() == other->isWeak()
                && exchange->getSuccessOrdering() == other->getSuccessOrdering()
                && exchange->getFailureOrdering() == other->getFailureOrdering()
                && exchange->getSyncScopeID() == other->getSyncScopeID()
            ? nullptr
            : "cmpxchg shape";
    }

    if (auto *modify = llvm::dyn_cast<llvm::AtomicRMWInst>(&left)) {
        auto *other = llvm::cast<llvm::AtomicRMWInst>(&right);

        return modify->getOperation() == other->getOperation()
                && modify->isVolatile() == other->isVolatile()
                && modify->getOrdering() == other->getOrdering()
                && modify->getSyncScopeID() == other->getSyncScopeID()
            ? nullptr
            : "atomicrmw shape";
    }

    if (auto *shuffle = llvm::dyn_cast<llvm::ShuffleVectorInst>(&left)) {
        return shuffle->getShuffleMask() == llvm::cast<llvm::ShuffleVectorInst>(&right)->getShuffleMask()
            ? nullptr
            : "shuffle mask";
    }

    return nullptr;
}

const char *Comparison::compare_instruction(const llvm::Instruction &left, const llvm::Instruction &right)
{
    if (left.getOpcode() != right.getOpcode()) {
        return "opcode";
    }

    if (!same_type(left.getType(), right.getType())) {
        return "result type";
    }

    if (left.getNumOperands() != right.getNumOperands()) {
        return "operand count";
    }

    // nuw, nsw, exact, a GEP's nowrap bits and the fast-math flags all live in this one word rather
    // than behind an accessor each
    if (left.getRawSubclassOptionalData() != right.getRawSubclassOptionalData()) {
        return "instruction flags";
    }

    if (const char *what = compare_operation(left, right)) {
        return what;
    }

    for (unsigned i = 0; i < left.getNumOperands(); i++) {
        if (!same_value(left.getOperand(i), right.getOperand(i))) {
            return "operand";
        }
    }

    if (const char *what = compare_attached_metadata(left, right)) {
        return what;
    }

    return compare_debug_records(left, right);
}

const char *Comparison::compare_signature(const llvm::Function &left, const llvm::Function &right)
{
    if (!same_type(left.getFunctionType(), right.getFunctionType())) {
        return "signature";
    }

    if (left.getLinkage() != right.getLinkage()) {
        return "linkage";
    }

    if (left.getVisibility() != right.getVisibility()) {
        return "visibility";
    }

    if (left.getUnnamedAddr() != right.getUnnamedAddr()) {
        return "unnamed_addr";
    }

    if (left.getCallingConv() != right.getCallingConv()) {
        return "calling convention";
    }

    // an AttributeList is uniqued in the shared context, so this compares the whole set exactly -
    // which is what a rendered `#0` never could, that number being a position in a per-module table
    if (left.getAttributes() != right.getAttributes()) {
        return "attributes";
    }

    if (left.getAlign() != right.getAlign()) {
        return "alignment";
    }

    if (left.getSection() != right.getSection()) {
        return "section";
    }

    // a Comdat belongs to its module, so the selection kind and the name are what carry across
    if ((left.getComdat() == nullptr) != (right.getComdat() == nullptr)) {
        return "comdat";
    }

    if (left.getComdat() != nullptr
        && (left.getComdat()->getName() != right.getComdat()->getName()
            || left.getComdat()->getSelectionKind() != right.getComdat()->getSelectionKind())) {
        return "comdat";
    }

    // the function's own attachments, `!dbg` above all - which is the subprogram describing it
    return compare_attached_metadata(left, right);
}

std::optional<Compiler::LLVM::OdrDifference> Comparison::run(
    const llvm::Function &left,
    const llvm::Function &right
)
{
    if (const char *what = compare_signature(left, right)) {
        return Compiler::LLVM::OdrDifference{ what, nullptr, nullptr };
    }

    number_body(left, _left_serials);
    number_body(right, _right_serials);

    if (left.size() != right.size()) {
        return Compiler::LLVM::OdrDifference{ "block count", nullptr, nullptr };
    }

    // in written order rather than depth first: both bodies come out of one emitter from one
    // declaration, so the order the blocks were created in is content too, and comparing them
    // positionally is stricter than following the successors
    auto left_block = left.begin();
    auto right_block = right.begin();

    for (size_t block_index = 0; left_block != left.end(); ++left_block, ++right_block, block_index++) {
        if (left_block->size() != right_block->size()) {
            return Compiler::LLVM::OdrDifference{
                fmt::format("block {} instruction count", block_index), nullptr, nullptr
            };
        }

        auto left_inst = left_block->begin();
        auto right_inst = right_block->begin();

        for (size_t index = 0; left_inst != left_block->end(); ++left_inst, ++right_inst, index++) {
            if (const char *what = compare_instruction(*left_inst, *right_inst)) {
                return Compiler::LLVM::OdrDifference{
                    fmt::format("block {}, instruction {}: {}", block_index, index, what),
                    &*left_inst,
                    &*right_inst
                };
            }
        }
    }

    return std::nullopt;
}

};

namespace Compiler::LLVM
{

std::optional<OdrDifference> first_odr_difference(const llvm::Function &left, const llvm::Function &right)
{
    Comparison comparison;

    return comparison.run(left, right);
}

};
