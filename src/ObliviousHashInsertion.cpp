#include "ObliviousHashInsertion.h"
#include "FunctionCallSitesInformation.h"
#include "Utils.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <assert.h>
#include <boost/algorithm/string/classification.hpp> // Include boost::for is_any_of
#include <boost/algorithm/string/split.hpp>          // Include for boost::split
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <sstream>
#include <string>

#include "../../self-checksumming/src/FunctionFilter.h"
#include "../../self-checksumming/src/FunctionMarker.h"

#include "input-dependency/FunctionInputDependencyResultInterface.h"

using namespace llvm;
namespace oh {

namespace {
unsigned get_random(unsigned range) { return rand() % range; }

bool skipInstruction(llvm::Instruction& I,
                     const input_dependency::InputDependencyAnalysisInterface::InputDepResType& F_input_dependency_info)
{
    if (auto phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
        return true;
    }
    if (auto callInst = llvm::dyn_cast<llvm::CallInst>(&I)) {
        auto calledF = callInst->getCalledFunction();
        if (calledF && (calledF->getName() == "assert"
            || calledF->getName() == "hash1" || calledF->getName() == "hash2")) {
            return true;
        }
        // evaluate dependency of call load arguments
    }
    return false;
}

class FunctionExtractionHelper
{
public:
    using SkipInstructionsPred = std::function<bool (llvm::Instruction* instr)>;
    FunctionExtractionHelper(Slicer& slicer,
                             const FunctionOHPaths::OHPath& path,
                             llvm::Function* F,
                             llvm::Function* assert_F,
                             llvm::Function* assert,
                             llvm::Function* hashFunc1,
                             llvm::Function* hashFunc2);

    void extractFunction();

    llvm::Function* getExtractedFunction()
    {
        return m_pathF;
    }

private:
    llvm::Function* createPathFunction();
    void createPathBlocks();
    void extractSlice(const Slicer::Slice& slice);
    void replaceUniqueAssertCall(llvm::CallInst* callInst);
    void removeArgumentReachableInstructions();
    void removeArgumentReachableInstruction(llvm::Instruction* inst);
    void adjustBlockTerminators();
    bool isHashCall(llvm::CallInst* callInst);
    bool isGlobalHashInstr(llvm::CallInst* callInst);

    static std::string getPathFunctionName(const std::string& assert_name);

private:
    Slicer& m_slicer;
    const FunctionOHPaths::OHPath& m_path;
    llvm::Function* m_F;
    llvm::Function* m_assertF;
    llvm::Function* m_assert;
    llvm::Function* m_hashFunc1;
    llvm::Function* m_hashFunc2;
    llvm::Function* m_pathF;
    llvm::ValueToValueMapTy m_valueMap;
};

FunctionExtractionHelper::FunctionExtractionHelper(Slicer& slicer,
                                                   const FunctionOHPaths::OHPath& path,
                                                   llvm::Function* F,
                                                   llvm::Function* assert_F,
                                                   llvm::Function* assert,
                                                   llvm::Function* hashF1,
                                                   llvm::Function* hashF2)
    : m_slicer(slicer)
    , m_path(path)
    , m_F(F)
    , m_assertF(assert_F)
    , m_assert(assert)
    , m_hashFunc1(hashF1)
    , m_hashFunc2(hashF2)
    , m_pathF(nullptr)
{
}

void FunctionExtractionHelper::extractFunction()
{
    m_slicer.slice(m_F, m_assertF->getName());
    const Slicer::Slice& slice = m_slicer.getSlice();
    // TODO: for debug only. Remove later
    //llvm::dbgs() << "Refine path function of " << m_F->getName() << "  " << m_assertF->getName() << "\n";
    //for (auto I : slice) {
    //    llvm::dbgs() << *I << "\n";
    //}

    llvm::Module* M = m_F->getParent();
    createPathFunction();
    llvm::dbgs() << "Extract path function " << m_pathF->getName() << "\n";
    createPathBlocks();
    extractSlice(slice);
    removeArgumentReachableInstructions();
    adjustBlockTerminators();

    llvm::SmallVector<llvm::BasicBlock*, 100> extracted_blocks;
    for (auto& B : *m_pathF) {
        extracted_blocks.push_back(&B);
    }
    llvm::remapInstructionsInBlocks(extracted_blocks, m_valueMap);
    // TODO: for debug only. Remove later
    //llvm::dbgs() << "After refining and remap the slice\n";
    //m_pathF->dump();
    m_assertF->eraseFromParent();
}

void FunctionExtractionHelper::createPathBlocks()
{
    for (auto& path_B : m_path) {
        llvm::BasicBlock* block = llvm::BasicBlock::Create(m_pathF->getParent()->getContext(), path_B->getName(), m_pathF);
        m_valueMap.insert(std::make_pair(path_B, llvm::WeakVH(block)));
    }
}

void FunctionExtractionHelper::extractSlice(const Slicer::Slice& slice)
{
    // Clone instructions to corresponding blocks
    for (auto I : slice) {
        //llvm::dbgs() << *I << "\n";
        // skip instructions that are outside of the path
        if (!FunctionOHPaths::pathContainsBlock(m_path, I->getParent())) {
            continue;
        }
        if (llvm::dyn_cast<llvm::ReturnInst>(I)) {
            continue;
        }
        if (auto* callInst = llvm::dyn_cast<llvm::CallInst>(I)) {
            if (isGlobalHashInstr(callInst)) {
                continue;
            }
            llvm::Function* calledF = callInst->getCalledFunction();
            if (calledF == m_assertF) {
                replaceUniqueAssertCall(callInst);
            }
        }
        auto* block = llvm::dyn_cast<llvm::BasicBlock>(m_valueMap[I->getParent()]);
        if (!block) {
            continue;
        }
        auto* cloned_I = I->clone();
        block->getInstList().push_back(cloned_I);
        m_valueMap.insert(std::make_pair(I, llvm::WeakVH(cloned_I)));
    }
}

void FunctionExtractionHelper::replaceUniqueAssertCall(llvm::CallInst* callInst)
{
    // assertion call looks like following
    // %0 = load i64, i64* @global
    // call void @assert(i64 %0, i64 placeholder)
    llvm::LoadInst* global_load = llvm::dyn_cast<llvm::LoadInst>(callInst->getArgOperand(0));
    callInst->setCalledFunction(m_assert);
    callInst->setArgOperand(0, global_load->getPointerOperand());
    auto pos = m_valueMap.find(global_load);
    if (pos != m_valueMap.end()) {
        if (auto* inst = llvm::dyn_cast<llvm::Instruction>(pos->second)) {
            inst->eraseFromParent();
            m_valueMap.erase(pos);
        }
    }
    global_load->eraseFromParent();
}

void FunctionExtractionHelper::removeArgumentReachableInstructions()
{
    dg::LLVMDependenceGraph* F_dg = m_slicer.getDG(m_F);
    auto arg_it = m_F->arg_begin();
    while (arg_it != m_F->arg_end()) {
        std::list<dg::LLVMNode*> dg_nodes;
        auto arg_node = F_dg->getNode(&*arg_it);
        if (arg_node == nullptr) {
            ++arg_it;
            continue;
        }
        dg_nodes.push_back(arg_node);
        std::unordered_set<dg::LLVMNode*> processed_nodes;
        while (!dg_nodes.empty()) {
            auto node = dg_nodes.back();
            dg_nodes.pop_back();
            if (!processed_nodes.insert(node).second) {
                continue;
            }
            //llvm::dbgs() << "dg node: " << *node->getValue() << "\n";
            auto dep_it = node->data_begin();
            if (!isHashCall(llvm::dyn_cast<llvm::CallInst>(node->getValue()))) {
                while (dep_it != node->data_end()) {
                    dg_nodes.push_back(*dep_it);
                    ++dep_it;
                }
            }
            removeArgumentReachableInstruction(llvm::dyn_cast<llvm::Instruction>(node->getValue()));
        }
        ++arg_it;
    }
}

void FunctionExtractionHelper::removeArgumentReachableInstruction(llvm::Instruction* inst)
{
    if (!inst) {
        return;
    }
    if (!FunctionOHPaths::pathContainsBlock(m_path, inst->getParent())) {
        return;
    }
    auto pos = m_valueMap.find(inst);
    if (pos == m_valueMap.end()) {
        return;
    }
    auto cloned_inst = llvm::dyn_cast<llvm::Instruction>(pos->second);
    assert(cloned_inst);
    cloned_inst->eraseFromParent();
    m_valueMap.erase(inst);
}

void FunctionExtractionHelper::adjustBlockTerminators()
{
    llvm::LLVMContext& Ctx = m_pathF->getParent()->getContext();
    llvm::BasicBlock* exit_block = llvm::BasicBlock::Create(Ctx, "exit", m_pathF);
    llvm::ReturnInst::Create(Ctx, exit_block);
    for (auto& block : m_path) {
        auto path_b = llvm::dyn_cast<llvm::BasicBlock>(m_valueMap[block]);
        auto path_term = path_b->getTerminator();
        if (path_term) {
            for (unsigned i = 0; i < path_term->getNumSuccessors(); ++i) {
                m_valueMap.insert(std::make_pair(path_term->getSuccessor(i), llvm::WeakVH(exit_block)));
            }
        } else {
            auto term = block->getTerminator();
            llvm::BasicBlock* dest = nullptr;
            for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
                dest = term->getSuccessor(i);
                if (m_valueMap.find(dest) != m_valueMap.end()) {
                    break;
                }
                dest = nullptr;
            }
            if (dest) {
                path_b->getInstList().push_back(llvm::BranchInst::Create(dest));
            } else {
                path_b->getInstList().push_back(llvm::BranchInst::Create(exit_block));
            }
        }
    }
}

bool FunctionExtractionHelper::isHashCall(llvm::CallInst* callInst)
{
    return callInst && (callInst->getCalledFunction() == m_hashFunc1 || callInst->getCalledFunction() != m_hashFunc2);
}

bool FunctionExtractionHelper::isGlobalHashInstr(llvm::CallInst* callInst)
{
    if (!callInst) {
        return false;
    }
    llvm::Function* calledF = callInst->getCalledFunction();
    if (calledF == m_assert) {
        return true;
    }
    if (calledF == m_hashFunc1 || calledF == m_hashFunc2) {
        // assuming global variables are used for global oh only
        return llvm::dyn_cast<llvm::GlobalVariable>(callInst->getOperand(0));
    }
    return false;
}

llvm::Function* FunctionExtractionHelper::createPathFunction()
{
    llvm::Module* M = m_F->getParent();
    llvm::FunctionType *function_type = llvm::FunctionType::get(llvm::Type::getVoidTy(M->getContext()),
                                                                llvm::ArrayRef<llvm::Type*>(), false);
    llvm::Constant* path_F = M->getOrInsertFunction(
                                                    getPathFunctionName(m_assertF->getName()),
                                                    function_type);
    m_pathF = llvm::dyn_cast<llvm::Function>(path_F);
}

std::string FunctionExtractionHelper::getPathFunctionName(const std::string& assert_name)
{
    // assert name looks like assert_....
    return assert_name.substr(7);
}

std::string get_path_function_name(const std::string& base_name, unsigned path_num)
{
    std::string f_name = base_name;
    f_name += "_path_";
    f_name += std::to_string(path_num);
    return f_name;
}

llvm::FunctionType* get_path_function_type(llvm::Function* F)
{
    const auto& params = F->getFunctionType()->params();
    return llvm::FunctionType::get(llvm::Type::getVoidTy(F->getContext()), params, false);
}

llvm::Function* get_assert_function_with_name(llvm::Module* M, const std::string& f_name)
{
    llvm::LLVMContext &Ctx = M->getContext();
    llvm::ArrayRef<llvm::Type *> params {llvm::Type::getInt64Ty(Ctx),
                                         llvm::Type::getInt64Ty(Ctx)};
    llvm::FunctionType *function_type = llvm::FunctionType::get(llvm::Type::getVoidTy(Ctx), params, false);
    return llvm::dyn_cast<llvm::Function>(M->getOrInsertFunction(f_name, function_type));
}

}

char ObliviousHashInsertionPass::ID = 0;
const std::string ObliviousHashInsertionPass::oh_path_functions_callee = "oh_path_functions";
const std::string GUARD_META_DATA = "guard";

static cl::opt<std::string>
    DumpOHStat("dump-oh-stat", cl::Hidden,
               cl::desc("File path to dump pass stat in Json format "));

static llvm::cl::opt<unsigned>
    num_hash("num-hash", llvm::cl::desc("Specify number of hash values to use"),
             llvm::cl::value_desc("num_hash"));

static llvm::cl::opt<std::string> SkipTaggedInstructions(
    "skip", llvm::cl::desc("Specify comma separated tagged instructios "
                           "(metadata) to be skipped by the OH pass"),
    llvm::cl::value_desc("skip"));

static cl::opt<bool>
    shortRangeOH("short-range-oh", cl::Hidden,
               cl::desc("Apply short range hashing"));

void ObliviousHashInsertionPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const
{
    AU.setPreservesAll();
    AU.addRequired<llvm::AssumptionCacheTracker>(); // otherwise run-time error
    llvm::getAAResultsAnalysisUsage(AU);
    AU.addRequired<input_dependency::InputDependencyAnalysisPass>();
    AU.addRequired<llvm::LoopInfoWrapperPass>();
    AU.addRequired<llvm::DominatorTreeWrapperPass>();
    AU.addRequired<FunctionCallSiteInformationPass>();
    AU.addRequired<FunctionMarkerPass>();
    AU.addRequired<FunctionFilterPass>();
}

std::map<Function *, int> checkeeMap;
unsigned int getUniqueCheckeeFunctionInstructionCount(llvm::Instruction &I) {
  if (auto *metadata = I.getMetadata(GUARD_META_DATA)) {
    auto strMD = llvm::dyn_cast<llvm::MDString>(metadata->getOperand(0).get());
    auto F = I.getModule()->getFunction(strMD->getString());
    if (F == NULL) {
      errs() << "Metadata failed to find function:" << strMD->getString()
             << "\n";
    }
    // if checkee is already counted skip it
    if (checkeeMap.find(F) != checkeeMap.end()) {
      dbgs() << "Metadata already seen:" << F->getName() << "\n";
      return 0;
    } else {
      dbgs() << "Metadata seen:" << strMD->getString() << "\n";
    }

    unsigned int count = 0;
    for (BasicBlock &bb : *F) {
      count = std::distance(bb.begin(), bb.end());
    }

    // add checkee to the map
    checkeeMap[F] = 1;
    return count;
  }
  return 0;
}

bool ObliviousHashInsertionPass::isInstAGuard(llvm::Instruction &I) {
  if (auto *metadata = I.getMetadata(GUARD_META_DATA)) {
    return true;
  }
  return false;
}
bool ObliviousHashInsertionPass::insertHash(llvm::Instruction &I,
                                            llvm::Value *v, 
                                            llvm::Value* hash_value,
                                            bool before)
{
    //#7 requires to hash pointer operand of a StoreInst
    /*if (v->getType()->isPointerTy()) {
      return;
      }*/
    llvm::LLVMContext &Ctx = I.getModule()->getContext();
    llvm::IRBuilder<> builder(&I);
    if (before)
        builder.SetInsertPoint(I.getParent(), builder.GetInsertPoint());
    else
        builder.SetInsertPoint(I.getParent(), ++builder.GetInsertPoint());
    return insertHashBuilder(builder, v, hash_value);
}

bool ObliviousHashInsertionPass::insertHashBuilder(llvm::IRBuilder<> &builder,
                                                   llvm::Value *v,
                                                   llvm::Value* hash_value)
{
    llvm::LLVMContext &Ctx = builder.getContext();
    llvm::Value *cast = nullptr;
    llvm::Value *load = nullptr;
    if (v->getType()->isPointerTy()) {
        llvm::Type *ptrType = v->getType()->getPointerElementType();
        if (!ptrType->isIntegerTy() && !ptrType->isFloatingPointTy()) {
            // Currently we only handle int and float pointers
            dbgs() << "Non numeric pointers (int and float) are skipped:"
                << *v << " " << *ptrType << "\n";
            return false;
        }
        load = builder.CreateLoad(v);
    } else {
        load = v;
    }

    if (load->getType()->isIntegerTy()) {
        cast = builder.CreateZExtOrBitCast(load, llvm::Type::getInt64Ty(Ctx));
    } else if (load->getType()->isFloatingPointTy()) {
        cast = builder.CreateFPToSI(load, llvm::Type::getInt64Ty(Ctx));
    } else {
        dbgs() << "\nERR. Any value other than int and float is passed to insertHashBuilder\n";
        dbgs() << *load->getType() << "\n";
        return false;
    }
    std::vector<llvm::Value *> arg_values;
    arg_values.push_back(hash_value);
    arg_values.push_back(cast);
    llvm::ArrayRef<llvm::Value *> args(arg_values);

    builder.CreateCall(get_random(2) ? hashFunc1 : hashFunc2, args);
    return true;
}

void ObliviousHashInsertionPass::parse_skip_tags() {
  if (!SkipTaggedInstructions.empty()) {
    boost::split(skipTags, SkipTaggedInstructions, boost::is_any_of(","),
                 boost::token_compress_on);
    hasTagsToSkip = true;
    llvm::dbgs() << "Noted " << SkipTaggedInstructions
                 << " as instruction tag(s) to skip\n";
  } else {
    llvm::dbgs() << "No tags were supplied to be skipped! \n";
    hasTagsToSkip = false;
  }
}

bool ObliviousHashInsertionPass::hasSkipTag(llvm::Instruction& I)
{
    // skip instrumenting instructions whose tag matches the skip tag list
    if (this->hasTagsToSkip && I.hasMetadataOtherThanDebugLoc()) {
        llvm::dbgs() << "Found instruction with tags ad we have set tags\n";
        for (auto tag : skipTags) {
            llvm::dbgs() << tag << "\n";
            if (auto *metadata = I.getMetadata(tag)) {
                llvm::dbgs() << "Skipping tagged instruction: " << I << "\n";
                return true;
            }
        }
    }
    return false;
}

bool ObliviousHashInsertionPass::instrumentInst(llvm::Instruction &I,
                                                llvm::Value* hash_to_update,
                                                bool is_local_hash)
{
    bool hashInserted = false;
    bool isCallGuard = false;
    int protectedArguments = 0;
    if (auto* cmp = llvm::dyn_cast<llvm::CmpInst>(&I)) {
        hashInserted = instrumentCmpInst(cmp, hash_to_update);
    } else if (llvm::ReturnInst::classof(&I)) {
        auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&I);
        if (auto *val = ret->getReturnValue()) {
            hashInserted = insertHash(I, val, hash_to_update, true);
        }
    } else if (llvm::LoadInst::classof(&I)) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&I);
        if (load->getPointerOperand() != hash_to_update
                && (!is_local_hash || !llvm::dyn_cast<llvm::Argument>(load->getPointerOperand()))) {
            hashInserted = insertHash(I, load, hash_to_update, false);
        }
    } else if (llvm::StoreInst::classof(&I)) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&I);
        llvm::Value *v = store->getPointerOperand();
        if (v != hash_to_update
                && (!is_local_hash || !llvm::dyn_cast<llvm::Argument>(store->getValueOperand()))) {
            hashInserted = insertHash(I, v, hash_to_update, false);
        }
    } else if (llvm::BinaryOperator::classof(&I)) {
        auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(&I);
        if (bin->getOpcode() == llvm::Instruction::Add) {
            hashInserted = insertHash(I, bin, hash_to_update, false);
        }
    } else if (auto *call = llvm::dyn_cast<llvm::CallInst>(&I)) {
        hashInserted = instrumentCallInst(call, protectedArguments, hash_to_update);
        isCallGuard = isInstAGuard(I);
    } else if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
        hashInserted = instrumentCallInst(invoke, protectedArguments, hash_to_update);
        isCallGuard = isInstAGuard(I);
    } else if (auto* getElemPtr = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
        hashInserted = instrumentGetElementPtrInst(getElemPtr, hash_to_update);
    } else {
        stats.addNumberOfNonHashableInstructions(1);
        llvm::dbgs() << "Non hashable instruction " << I << "\n"; 
    }

    if (hashInserted) {
        is_local_hash ? stats.addNumberOfShortRangeHashCalls(1) : stats.addNumberOfHashCalls(1);
        is_local_hash ? stats.addNumberOfShortRangeProtectedInstructions(1) : stats.addNumberOfProtectedInstructions(1);
        if (isCallGuard) {
            // When call is a guard we compute implicit OH stats
            // See #37
            unsigned int checkeeSize = getUniqueCheckeeFunctionInstructionCount(I);
            if (checkeeSize > 0) {
                dbgs()<<"Implicit protection instructions to add:"<<checkeeSize<<"\n";
            }
            is_local_hash ? stats.addNumberOfShortRangeImplicitlyProtectedInstructions(checkeeSize)
                          : stats.addNumberOfImplicitlyProtectedInstructions(checkeeSize);

            //#20 increment number of protected guard instruction,
            // when at least one of the guard call arguments are
            // incorporated into the hash
            is_local_hash ? stats.addNumberOfShortRangeProtectedGuardInstructions(1) : stats.addNumberOfProtectedGuardInstructions(1);
            is_local_hash ? stats.addNumberOfShortRangeProtectedGuardArguments(protectedArguments)
                       : stats.addNumberOfProtectedGuardArguments(protectedArguments);
        } else {
            is_local_hash ? stats.addNumberOfShortRangeProtectedArguments(protectedArguments) : stats.addNumberOfProtectedArguments(protectedArguments);
        }
    }

    /// END STATS

    return hashInserted;
}

template <class CallInstTy>
bool ObliviousHashInsertionPass::instrumentCallInst(CallInstTy* call, 
                                                    int& protectedArguments,
                                                    llvm::Value* hash_value)
{
    bool hashInserted = false;
    llvm::dbgs() << "Processing call instruction..\n";
    call->dump();
    auto called_function = call->getCalledFunction();
    if (called_function == nullptr || called_function->isIntrinsic() ||
            called_function == hashFunc1 || called_function == hashFunc2) {
        return false;
    }

    for (int i = 0; i < call->getNumArgOperands(); ++i) {
        auto* operand = call->getArgOperand(i);
        bool argHashed = false;
        if (auto *const_op = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
            llvm::dbgs() << "***Handling a call instruction***\n";
            argHashed = insertHash(*call, const_op, hash_value, false);
            if (!argHashed) {
                errs() << "ERR. constant int argument passed to insert hash, but "
                    "failed to hash\n";
                const_op->dump();
            } else {
                ++protectedArguments;
            }
        } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(operand)) {
            if (!m_input_dependency_info->isInputDependent(load)) {
                argHashed = insertHash(*load, load, hash_value, false);
                if (argHashed) {
                    ++protectedArguments;
                } else {
                    errs() << "ERR. constant int argument passed to insert hash, but "
                        "failed to hash\n";
                    load->dump();
                }
            } else {
                llvm::dbgs() << "Can't handle input dependent load operand ";
                load->dump();
            }
        } else {
            llvm::dbgs() << "Can't handle this operand " << *operand
                         << " of the call " << *call << "\n";
        }
        hashInserted = hashInserted || argHashed;
    }
    return hashInserted;
}

bool ObliviousHashInsertionPass::instrumentGetElementPtrInst(llvm::GetElementPtrInst* getElemPtr,
                                                             llvm::Value* hash_value)
{
    bool hashInserted = insertHash(*getElemPtr, getElemPtr, hash_value, false);
    // hash constant and input independent indices
    auto idx_it = getElemPtr->idx_begin();
    while (idx_it != getElemPtr->idx_end()) {
        if (auto* constant_idx = llvm::dyn_cast<llvm::Constant>(*idx_it)) {
            hashInserted |= insertHash(*getElemPtr, constant_idx, hash_value, false);
        } else if (auto* load_inst = llvm::dyn_cast<llvm::LoadInst>(*idx_it)) {
            if (!m_input_dependency_info->isInputDependent(load_inst)) {
                hashInserted |= insertHash(*getElemPtr, load_inst, hash_value, false);
            }
        }
        ++idx_it;
    }
    return hashInserted;
}

bool ObliviousHashInsertionPass::instrumentCmpInst(llvm::CmpInst* I, llvm::Value* hash_value)
{
    llvm::LLVMContext &Ctx = I->getModule()->getContext();
    llvm::IRBuilder<> builder(I);
    builder.SetInsertPoint(I->getParent(), ++builder.GetInsertPoint());

    // Insert the transformation of the cmp output into something more usable by
    // the hash function.
    llvm::Value *cmpExt =
        builder.CreateZExtOrBitCast(I, llvm::Type::getInt8Ty(Ctx));
    llvm::Value *val = builder.CreateAdd(
            builder.CreateMul(
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(Ctx), 64),
                builder.CreateAdd(
                    cmpExt, llvm::ConstantInt::get(llvm::Type::getInt8Ty(Ctx), 1))),
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(Ctx),
                I->getPredicate()));
    auto *AddInst = dyn_cast<Instruction>(val);
    bool hashInserted = insertHash(*AddInst, val, hash_value, false);
    if (!hashInserted) {
        errs() << "Potential ERR: insertHash failed for";
        val->dump();
    }
    return hashInserted;
}

void ObliviousHashInsertionPass::insertAssert(llvm::Instruction &I,
                                              llvm::Value* hash_to_assert,
                                              bool short_range_assert,
                                              llvm::Constant* assert_F)
{
    llvm::LLVMContext &Ctx = I.getModule()->getContext();
    llvm::IRBuilder<> builder(&I);
    builder.SetInsertPoint(I.getParent(), builder.GetInsertPoint());
    // assert for cmp instruction should have been added
    if (llvm::CmpInst::classof(&I)) {
        // log the last value which contains hash for this cmp instruction
        // builder.SetInsertPoint(I.getParent(), ++builder.GetInsertPoint());
        insertAssert(builder, I, hash_to_assert, short_range_assert, assert_F);
        return;
    }
    if (auto callInst = llvm::dyn_cast<llvm::CallInst>(&I)) {
        auto called_function = callInst->getCalledFunction();
        if (called_function != nullptr && !called_function->isIntrinsic() &&
                called_function != hashFunc1 && called_function != hashFunc2) {
            // always insert assert before call instructions
            insertAssert(builder, I, hash_to_assert, short_range_assert, assert_F);
            return;
        }
    }
    if (short_range_assert) {
        insertAssert(builder, I, hash_to_assert, true, assert_F);
    } else if (get_random(2)) {
        // insert randomly
        insertAssert(builder, I, hash_to_assert, false, assert_F);
    }
}

void ObliviousHashInsertionPass::insertAssert(llvm::IRBuilder<> &builder,
                                              llvm::Instruction &instr,
                                              llvm::Value* hash_value,
                                              bool short_range_assert,
                                              llvm::Constant* assert_F)
{
    llvm::LLVMContext &Ctx = builder.getContext();
    builder.SetInsertPoint(instr.getParent(), builder.GetInsertPoint());

    ConstantInt *const_int = (ConstantInt *)ConstantInt::get(
            Type::getInt64Ty(Ctx), APInt(64, (assertCnt)* 1000000000000));
    std::vector<Value *> values; 
    llvm::LoadInst* hash_load = nullptr;
    if(short_range_assert){
        auto loaded_local_hash = builder.CreateLoad(hash_value);
        builder.CreateStore(loaded_local_hash, TempVariable);
        hash_load = builder.CreateLoad(TempVariable);
        values = {hash_load, const_int};
    } else {
        values = {hash_value, const_int};
    }
    ArrayRef<Value *> args(values);
    assertCnt++;
    // Stats add the assert call
    short_range_assert ? stats.addNumberOfShortRangeAssertCalls(1) : stats.addNumberOfAssertCalls(1);


    builder.CreateCall(assert_F, args);
}

void ObliviousHashInsertionPass::setup_guardMe_metadata()
{
    guardMetadataKindID = m_M->getMDKindID(GUARD_META_DATA);
    if (guardMetadataKindID > 0) {
        llvm::dbgs() << "'guard' metadata was found in the input bitcode\n";
    } else {
        llvm::dbgs() << "No 'guard' metadata was found\n";
    }
}

void ObliviousHashInsertionPass::setup_used_analysis_results()
{
    m_input_dependency_info = getAnalysis<input_dependency::InputDependencyAnalysisPass>().getInputDependencyAnalysis();
    m_function_mark_info = getAnalysis<FunctionMarkerPass>().get_functions_info();
    llvm::dbgs() << "Recieved marked functions "
                 << m_function_mark_info->get_functions().size() << "\n";
    m_function_filter_info = getAnalysis<FunctionFilterPass>().get_functions_info();
    llvm::dbgs() << "Recieved filter functions "
                 << m_function_filter_info->get_functions().size() << "\n";
    m_function_callsite_data = &getAnalysis<FunctionCallSiteInformationPass>().getAnalysisResult();
}

void ObliviousHashInsertionPass::setup_functions()
{
    // Get the function to call from our runtime library.
    llvm::LLVMContext &Ctx = m_M->getContext();
    llvm::ArrayRef<llvm::Type *> params {llvm::Type::getInt64PtrTy(Ctx),
                                         llvm::Type::getInt64Ty(Ctx)};
    llvm::FunctionType *function_type = llvm::FunctionType::get(llvm::Type::getVoidTy(Ctx), params, false);
    hashFunc1 = llvm::dyn_cast<llvm::Function>(m_M->getOrInsertFunction("hash1", function_type));
    hashFunc2 = llvm::dyn_cast<llvm::Function>(m_M->getOrInsertFunction("hash2", function_type));
    assert = llvm::dyn_cast<llvm::Function>(m_M->getOrInsertFunction("assert", function_type));
}

void ObliviousHashInsertionPass::setup_hash_values()
{
    // Insert Global hash variables
    hashPtrs.reserve(num_hash);
    stats.setNumberOfHashVariables(num_hash);

    llvm::LLVMContext &Ctx = m_M->getContext();
    for (int i = 0; i < num_hash; i++) {
        hashPtrs.push_back(new llvm::GlobalVariable(
                    *m_M, llvm::Type::getInt64Ty(Ctx), false,
                    llvm::GlobalValue::ExternalLinkage,
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), 0)));
    }
    TempVariable = new llvm::GlobalVariable(
		         *m_M, llvm::Type::getInt64Ty(Ctx), false,
		         llvm::GlobalValue::ExternalLinkage,
		         llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), 0));
}

bool ObliviousHashInsertionPass::skip_function(llvm::Function& F) const
{
    if (F.isDeclaration() || F.isIntrinsic()) {
        return true;
    }
    if (m_function_filter_info->get_functions().size() != 0 && !m_function_filter_info->is_function(&F)) {
        llvm::dbgs() << " Skipping function per FilterFunctionPass:" << F.getName() << "\n";
        return true;
    }
    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(&F);
    if (!F_input_dependency_info) {
        llvm::dbgs() << "Skipping function. No input dep info " << F.getName() << "\n";
        return true;
    }
    return false;
}

bool ObliviousHashInsertionPass::process_function(llvm::Function* F)
{

    llvm::dbgs() << " Processing function:" << F->getName() << "\n";
    stats.addNumberOfSensitiveBlocks(F->getBasicBlockList().size());
    if (shortRangeOH) {
        llvm::dbgs() << "Short range hashing enabled.\n";
        return process_function_with_short_range_oh_enabled(F);
    }
    return process_function_with_global_oh(F);
}

bool ObliviousHashInsertionPass::process_function_with_short_range_oh_enabled(llvm::Function* F)
{
    bool modified = false;
    llvm::DominatorTree& domTree = getAnalysis<llvm::DominatorTreeWrapperPass>(*F).getDomTree();
    FunctionOHPaths paths(F, &domTree);
    paths.constructOHPaths();

    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    llvm::LoopInfo &LI = getAnalysis<llvm::LoopInfoWrapperPass>(*F).getLoopInfo();
    stats.addNumberOfSensitivePaths(paths.size());
    for (unsigned i = 0; i < paths.size(); ++i) {
        modified |= process_path(F, paths[i], i);
    }

    return modified;
}

bool ObliviousHashInsertionPass::process_function_with_global_oh(llvm::Function* F)
{
    bool modified = false;
    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    if (F_input_dependency_info->isInputDepFunction()) {
        llvm::dbgs() << "Skip input dependent function " << F->getName() << "\n";
        return modified;
    }
    llvm::LoopInfo &LI = getAnalysis<llvm::LoopInfoWrapperPass>(*F).getLoopInfo();
    for (auto& B : *F) {
        bool can_insert_assertions = can_insert_assertion_at_location(F, &B, LI);
        if (F_input_dependency_info->isInputDependentBlock(&B)) {
            continue;
        }
        modified |= process_block(F, &B, can_insert_assertions, [] (llvm::Instruction* instr) {return false;});
    }
    return modified;
}

void ObliviousHashInsertionPass::insert_calls_for_path_functions()
{
    llvm::FunctionType *function_type = llvm::FunctionType::get(llvm::Type::getVoidTy(m_M->getContext()),
                                                                llvm::ArrayRef<llvm::Type*>(), false);
    auto* path_functions_callee = llvm::dyn_cast<llvm::Function>(m_M->getOrInsertFunction(oh_path_functions_callee,
                                                                                          function_type));
    llvm::LLVMContext& Ctx = path_functions_callee->getContext();
    llvm::BasicBlock* entry_block = llvm::BasicBlock::Create(Ctx,
                                                             "entry",
                                                             path_functions_callee);
    llvm::IRBuilder<> builder(Ctx);
    for (auto& path_F : m_path_functions) {
        builder.SetInsertPoint(entry_block);
        builder.CreateCall(path_F, llvm::ArrayRef<llvm::Value*>());
    }
    llvm::ReturnInst::Create(Ctx, entry_block);

    llvm::Function* mainF = m_M->getFunction("main");
    if (!mainF) {
        llvm::dbgs() << "No main function. Can not insert calls to extracted path functions\n";
        return;
    }
    llvm::IRBuilder<> main_builder(mainF->getContext());
    main_builder.SetInsertPoint(&*mainF->getEntryBlock().getFirstInsertionPt());
    main_builder.CreateCall(path_functions_callee, llvm::ArrayRef<llvm::Value*>());
}

bool ObliviousHashInsertionPass::process_path(llvm::Function* F,
                                              const FunctionOHPaths::OHPath& path,
                                              unsigned path_num)
{
    bool modified = false;
    llvm::dbgs() << "Processing path: ";
    for (const auto& B : path) {
        llvm::dbgs() << B->getName() << "  ";
    }
    llvm::dbgs() << "\n";
    bool can_process_nondet_path = can_process_path(F, path);
    if (!can_process_nondet_path) {
        llvm::dbgs() << "Can not process non-deterministic part of the path\n";
    }
    llvm::BasicBlock* entry_block = path.front();
    llvm::LLVMContext &Ctx = entry_block->getContext();
    auto local_hash = new llvm::AllocaInst(llvm::Type::getInt64Ty(Ctx), 0,8, "local_hash");
    auto alloca_pos = entry_block->getInstList().insert(entry_block->begin(), local_hash);
    auto local_store = new llvm::StoreInst(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), 0), local_hash, false, 8);
    entry_block->getInstList().insertAfter(alloca_pos, local_store);

    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    const auto& skip_instruction_pred = [&local_hash, &local_store, &F_input_dependency_info] (llvm::Instruction* instr)
                                         {
                                            return instr == local_hash || instr == local_store;
                                         };

    llvm::LoopInfo &LI = getAnalysis<llvm::LoopInfoWrapperPass>(*F).getLoopInfo();
    bool has_inputdep_block = false;
    bool local_hash_updated = false;
    for (auto& B : path) {
        if (!F_input_dependency_info->isInputDependentBlock(B) && !F_input_dependency_info->isInputDepFunction()) {
            bool can_insert_assertions = can_insert_assertion_at_location(F, B, LI);
            if (m_processed_deterministic_blocks.insert(B).second) {
                modified |= process_block(F, B, can_insert_assertions, skip_instruction_pred);
            }
        } else if (can_process_nondet_path) {
            modified |= process_path_block(F, B, local_hash, (B == path.back()),
                                           skip_instruction_pred, local_hash_updated,
                                           path_num);
            has_inputdep_block = true;
        }
    }
    if (!can_process_nondet_path || !modified || !has_inputdep_block) {
        llvm::dbgs() << "No oh has been applied in the path\n";
        local_store->eraseFromParent();
        local_hash->eraseFromParent();
    }
    if (can_process_nondet_path && modified) {
        stats.addNumberOfProtectedPaths(1);
        if (m_path_assertions.find(F) != m_path_assertions.end() && !m_path_assertions[F].empty()) {
            m_function_path.insert(std::make_pair(m_path_assertions[F].back(), path));
        }
    }
    return modified;
}

void ObliviousHashInsertionPass::extract_path_functions()
{
    m_slicer.reset(new Slicer(m_M));
    for (const auto& F_asserts : m_path_assertions) {
        //llvm::dbgs() << "F: " << F_asserts.first->getName() << "\n";
        for (const auto& F_assert : F_asserts.second) {
            FunctionExtractionHelper pathExtractor(*m_slicer,
                                                   m_function_path[F_assert],
                                                   F_asserts.first,
                                                   F_assert,
                                                   assert,
                                                   hashFunc1,
                                                   hashFunc2);
            pathExtractor.extractFunction();
            m_path_functions.push_back(pathExtractor.getExtractedFunction());
        }
    }
    // after extraction all path assertion functions will be deleted
    m_path_assertions.clear();
}

bool ObliviousHashInsertionPass::can_instrument_instruction(llvm::Function* F,
                                                            llvm::Instruction* I,
                                                            const SkipFunctionsPred& skipInstructionPred)
{
    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    if (skipInstruction(*I, F_input_dependency_info) || skipInstructionPred(I)) {
        return false;
    }
    if (hasSkipTag(*I)) {
        return false;
    }
    // TODO: make this change after discussing
    //if (!F_input_dependency_info->isInputDependent(I)) {
    //    if (!F_input_dependency_info->isInputIndependent(I)) {
    //        return false;
    //    }
    //    return true;
    //}
    //return !F_input_dependency_info->isDataDependent(I);
    return !F_input_dependency_info->isInputDependent(I) || !F_input_dependency_info->isDataDependent(I);
}

bool ObliviousHashInsertionPass::process_path_block(llvm::Function* F, llvm::BasicBlock* B,
                                               llvm::Value* hash_value, bool insert_assert,
                                               const SkipFunctionsPred& skipInstructionPred,
                                               bool& local_hash_updated,
                                               int path_num)
{
    bool modified = false;
    assert(hash_value);
    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    for (auto &I : *B) {
        if (can_instrument_instruction(F, &I, skipInstructionPred)) {
            local_hash_updated |= instrumentInst(I, hash_value, true);
            modified |= local_hash_updated;
        }
        if (!insert_assert || !local_hash_updated || &I != B->getTerminator()) {
            continue;
        }
        const std::string& assert_name = "assert_" + get_path_function_name(F->getName(), path_num);
        llvm::Function* path_assert = get_assert_function_with_name(F->getParent(), assert_name);
        m_path_assertions[F].push_back(path_assert);
        insertAssert(I, hash_value, true, path_assert);
    }
    if (modified) {
        stats.addNumberOfShortRangeProtectedBlocks(1);
    }
    return modified;
}

bool ObliviousHashInsertionPass::process_block(llvm::Function* F, llvm::BasicBlock* B,
                                               bool insert_assert, const SkipFunctionsPred& skipInstructionPred)
{
    bool modified = false;
    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    for (auto &I : *B) {
        if (!can_instrument_instruction(F, &I, skipInstructionPred)) {
            continue;
        }
        unsigned index = get_random(num_hash);
        usedHashIndices.push_back(index);
        llvm::Value* hash_to_update = hashPtrs.at(index);
        m_hashUpdated |= instrumentInst(I, hash_to_update, false);
        modified |= m_hashUpdated;
        if (!insert_assert || !m_hashUpdated || usedHashIndices.empty()) {
            llvm::dbgs() << "Insert assertion skipped because there was no hash "
                            "update in between!\n";
            continue;
        }
        llvm::Value* hash_to_assert = nullptr;
        if (llvm::CmpInst::classof(&I)) {
            hash_to_assert = hashPtrs.at(usedHashIndices.back());
        } else {
            unsigned random_hash_idx =
                usedHashIndices.at(get_random(usedHashIndices.size()));
            assert(random_hash_idx < hashPtrs.size());
            hash_to_assert = hashPtrs.at(random_hash_idx);
        }
        insertAssert(I, hash_to_assert, false, assert);
        m_hashUpdated = false;
    }
    if (modified) {
        stats.addNumberOfProtectedBlocks(1);
    }
    return modified;
}

bool ObliviousHashInsertionPass::can_process_path(llvm::Function* F, const FunctionOHPaths::OHPath& path)
{
    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    llvm::LoopInfo &LI = getAnalysis<llvm::LoopInfoWrapperPass>(*F).getLoopInfo();

    llvm::BasicBlock* assert_block = path.back();
    // if function is input dependent and assert block is inside a loop
    if (F_input_dependency_info->isInputDepFunction() && LI.getLoopFor(assert_block)) {
        llvm::dbgs() << "The last block of a path is in a loop. Can not insert assertion\n";
        return false;
    }
    // if outer loop of the path is input dependent and assert block is inside the loop
    for (auto& B : path) {
        if (LI.getLoopFor(B) && F_input_dependency_info->isInputDependentBlock(B)) {
            return false;
        }
    }
    return is_argument_used_in_path(F, path);
}

bool ObliviousHashInsertionPass::is_argument_used_in_path(llvm::Function* F,  const FunctionOHPaths::OHPath& path)
{

    dg::LLVMDependenceGraph* F_dg = m_slicer->getDG(F);
    if (!F_dg) {
        return false;
    }
    auto F_input_dependency_info = m_input_dependency_info->getAnalysisInfo(F);
    auto arg_it = F->arg_begin();
    while (arg_it != F->arg_end()) {
        std::list<dg::LLVMNode*> dg_nodes;
        auto arg_node = F_dg->getNode(&*arg_it);
        if (arg_node == nullptr) {
            ++arg_it;
            continue;
        }
        dg_nodes.push_back(arg_node);
        std::unordered_set<dg::LLVMNode*> processed_nodes;
        while (!dg_nodes.empty()) {
            auto node = dg_nodes.back();
            dg_nodes.pop_back();
            if (!processed_nodes.insert(node).second) {
                continue;
            }
            //llvm::dbgs() << "dg node: " << *node->getValue() << "\n";
            auto dep_it = node->data_begin();
            while (dep_it != node->data_end()) {
                dg_nodes.push_back(*dep_it);
                ++dep_it;
            }
            if (auto* inst = llvm::dyn_cast<llvm::Instruction>(node->getValue())) {
                if (!FunctionOHPaths::pathContainsBlock(path, inst->getParent())) {
                    continue;
                }
                if (auto* storeInst = llvm::dyn_cast<llvm::StoreInst>(inst)) {
                    if (llvm::dyn_cast<llvm::Argument>(storeInst->getValueOperand())) {
                        continue;
                    }
                }
                if ((F_input_dependency_info->isInputDepFunction() ||
                            F_input_dependency_info->isInputDependentBlock(inst->getParent()))
                    && !F_input_dependency_info->isDataDependent(inst)) {
                    llvm::dbgs() << "Data independent argument " << *arg_it << " is used in the path.\n";
                    return false;
                }
            }
        }
        ++arg_it;
    }
    return true;
}

bool ObliviousHashInsertionPass::can_insert_assertion_at_location(
                                  llvm::Function* F,
                                  llvm::BasicBlock* B,
                                  llvm::LoopInfo& LI)
{
    // We shouldn't insert asserts when there was no hash update, see #9
    
    if (m_function_callsite_data->isFunctionCalledInLoop(F)) {
        llvm::dbgs() << "Insert assertion skipped function:" << F->getName()
                     << " because it is called from a loop!\n";
        return false;
    }

    auto loop = LI.getLoopFor(B);
    if (loop != nullptr) {
        return false;
    }
    if (!m_function_mark_info->get_functions().empty() && m_function_mark_info->is_function(F)) {
        llvm::dbgs() << "Insert assertion skipped function:" << F->getName()
                     << " because it is in the skip assert list!\n";
        return false;
    }
    // Begin #24
    // If the function has multiple callsites it should get an assert with
    // multiple correct hash values, otherwise patcher keeps the last call
    // as the correct hash. Boom! This breaks OH and signals tampering...
    // for now we insert no asserts in functions with multiple callsites
    // moving forward we should get asserts with multiple place holders
    // There is already an issue for this - #22
    int callsites = m_function_callsite_data->getNumberOfFunctionCallSites(F);
    llvm::dbgs() << "Insert assertion evaluating function:" << F->getName()
                 << " callsites detected =" << callsites << "\n";
    if (callsites > 1) {
        llvm::dbgs() << "Insert assertion skipped function:" << F->getName()
                     << " because it has more than one call site, see #24.!\n";
        return false;
    }
    // END #24
    llvm::dbgs() << "Insert assertion included function:" << F->getName()
                 << " because it is not in the skip  assert list!\n";
    return true;
}

bool ObliviousHashInsertionPass::runOnModule(llvm::Module& M)
{
    llvm::dbgs() << "Insert hash computation\n";
    assertCnt = 1;
    bool modified = false;
    srand(time(NULL));
    m_M = &M;
    m_slicer.reset(new Slicer(m_M));

    parse_skip_tags();
    setup_guardMe_metadata();
    setup_used_analysis_results();
    setup_functions();
    setup_hash_values();

    llvm::Optional<llvm::BasicAAResult> BAR;
    llvm::Optional<llvm::AAResults> AAR;
    auto AARGetter = [&](llvm::Function* F) -> llvm::AAResults* {
        BAR.emplace(llvm::createLegacyPMBasicAAResult(*this, *F));
        AAR.emplace(llvm::createLegacyPMAAResults(*this, *F, *BAR));
        return &*AAR;
    };

    int countProcessedFuncs = 0;
    m_hashUpdated = false;
    for (auto &F : M) {
        if (skip_function(F)) {
            continue;
        }
        stats.addNumberOfSensitiveFunctions(1);
        ++countProcessedFuncs;
        m_AAR = AARGetter(&F);
        if (process_function(&F)) {
            stats.addNumberOfProtectedFunctions(1);
            modified = true;
        }
    }
    if (shortRangeOH) {
        extract_path_functions();
        insert_calls_for_path_functions();
    }

    if (!DumpOHStat.empty()) {
        dbgs() << "OH stats is requested, dumping stat file...\n";
        stats.dumpJson(DumpOHStat);
    }
    // Make sure OH only processed filter function list
    if (countProcessedFuncs != m_function_filter_info->get_functions().size()
        && m_function_filter_info->get_functions().size() > 0) {
        llvm::errs() << "ERR. processed " << countProcessedFuncs
                     << " function, while filter count is "
                     << m_function_filter_info->get_functions().size() << "\n";
    }
    return modified;
}

static llvm::RegisterPass<ObliviousHashInsertionPass>
    X("oh-insert", "Instruments bitcode with hashing and logging functions");

static void registerPathsAnalysisPass(const llvm::PassManagerBuilder &,
                                      llvm::legacy::PassManagerBase &PM) {

  PM.add(new ObliviousHashInsertionPass());
}

static llvm::RegisterStandardPasses
    RegisterMyPass(llvm::PassManagerBuilder::EP_EarlyAsPossible,
                   registerPathsAnalysisPass);
}
