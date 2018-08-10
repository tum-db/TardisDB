//
// Created by josef on 26.11.16.
//

#include "codegen/CodeGen.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/TypeBuilder.h>

// target data layout:
#if defined (__amd64__)
#define LAYOUT_DESCRIPTION "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
#else
#error "Layout description not available for this platform"
#endif

using namespace llvm;

thread_local CodeGen g_codeGen;

CodeGen & getThreadLocalCodeGen()
{
    return g_codeGen;
}

//-----------------------------------------------------------------------------

CodeGen::CodeGen() :
        context(),
        currentFunctionGen(nullptr),
        builder(context)
{ }

CodeGen::~CodeGen()
{
    assert(nextBBVec.empty());
}

void CodeGen::setNextBlock(BasicBlock * nextBB)
{
    assert(currentFunctionGen != nullptr);
    nextBBVec.push_back(nextBB);
}

void CodeGen::leaveBlock(BasicBlock * afterBB)
{
    assert(currentFunctionGen != nullptr);

    // get the next basic block or create a new one
    llvm::BasicBlock * nextBB = nullptr;
    if (nextBBVec.empty()) {
        nextBB = afterBB;
    } else {
        nextBB = nextBBVec.back();
        nextBBVec.pop_back();
    }

    builder.CreateBr(nextBB);
}

void CodeGen::setCurrentFunctionGen(FunctionGen & functionGen)
{
    currentFunctionGen = &functionGen;
}

void CodeGen::setCurrentModuleGen(ModuleGen & moduleGen)
{
    currentModuleGen = &moduleGen;
}

ModuleGen & CodeGen::getCurrentModuleGen()
{
    assert(currentModuleGen != nullptr);
    return *currentModuleGen;
}

void CodeGen::finalizeModuleGen()
{
    assert(currentModuleGen != nullptr);
    currentModuleGen = nullptr;
}

llvm::Module & CodeGen::getCurrentModule()
{
    return currentModuleGen->getModule();
}

FunctionGen & CodeGen::getCurrentFunctionGen()
{
    assert(currentFunctionGen != nullptr);
    return *currentFunctionGen;
}

void CodeGen::finalizeFunctionGen()
{
    assert(currentFunctionGen != nullptr);
    currentFunctionGen = nullptr;
}

llvm::Function & CodeGen::getCurrentFunction()
{
    return *currentFunctionGen->getFunction();
}

llvm::IRBuilder<> & CodeGen::getIRBuilder()
{
    return builder;
}

llvm::IRBuilder<> * CodeGen::operator->()
{
    return &builder;
}

llvm::LLVMContext & CodeGen::getLLVMContext()
{
    return context;
}

TypeCache & CodeGen::getTypeCache()
{
    return typeCache;
}

//-----------------------------------------------------------------------------

ModuleGen::ModuleGen(const std::string & moduleName) :
    context(g_codeGen.getLLVMContext()),
    codeGen(g_codeGen)
{
    assert(!g_codeGen.hasModuleGen());

    // initialize:
    module = make_unique<llvm::Module>(StringRef(moduleName), context);
    dataLayout = make_unique<llvm::DataLayout>(LAYOUT_DESCRIPTION);
    module->setDataLayout(*dataLayout);
    codeGen.setCurrentModuleGen(*this);
}

ModuleGen::~ModuleGen()
{
    codeGen.finalizeModuleGen();
}

llvm::LLVMContext & ModuleGen::getContext()
{
    return context;
}

llvm::Module & ModuleGen::getModule()
{
    return *module.get();
}

void ModuleGen::addFunctionMapping(Function * func, void * funcPtr)
{
    functionMapping.push_back( std::make_pair(func, funcPtr) );
}

std::unique_ptr<Module> ModuleGen::finalizeModule()
{
    return std::move(module);
}

void ModuleGen::applyMapping(llvm::ExecutionEngine * ee)
{
    for (const auto & mapping : functionMapping) {
        ee->addGlobalMapping(mapping.first, mapping.second);
    }
}

CodeGen & ModuleGen::getCodeGen()
{
    return codeGen;
}

llvm::DataLayout & ModuleGen::getDataLayout()
{
    return *dataLayout.get();
}

//-----------------------------------------------------------------------------

FunctionGen::FunctionGen(ModuleGen & moduleGen, llvm::StringRef name,
                         llvm::FunctionType * functionType) :
        moduleGen(moduleGen),
        function(nullptr),
        returnValue(nullptr)
{
    assert(!g_codeGen.hasFunctionGen());

    auto & context = moduleGen.getContext();
    auto & module = moduleGen.getModule();
    auto & codeGen = moduleGen.getCodeGen();

    // create a new function object
    function = cast<Function>(module.getOrInsertFunction(name, functionType));
    BasicBlock * entryBB = BasicBlock::Create(context, "EntryBlock", function);
    codeGen->SetInsertPoint(entryBB);

    moduleGen.getCodeGen().setCurrentFunctionGen(*this);
}

FunctionGen::FunctionGen(ModuleGen & moduleGen, llvm::StringRef name,
                         llvm::FunctionType * functionType, llvm::AttributeList attributeList) :
        moduleGen(moduleGen),
        function(nullptr),
        returnValue(nullptr)
{
    assert(!g_codeGen.hasFunctionGen());

    auto & context = moduleGen.getContext();
    auto & module = moduleGen.getModule();
    auto & codeGen = moduleGen.getCodeGen();

    // create a new function object
    function = cast<Function>(module.getOrInsertFunction(name, functionType, attributeList));
    BasicBlock * entryBB = BasicBlock::Create(context, "EntryBlock", function);
    codeGen->SetInsertPoint(entryBB);

    moduleGen.getCodeGen().setCurrentFunctionGen(*this);
}

FunctionGen::~FunctionGen()
{
    auto & codeGen = moduleGen.getCodeGen();

    if (returnValue == nullptr) {
        codeGen->CreateRetVoid();
    } else {
        codeGen->CreateRet(returnValue);
    }

    codeGen.finalizeFunctionGen();
}

ModuleGen & FunctionGen::getModuleGen()
{
    return moduleGen;
}

llvm::Function * FunctionGen::getFunction()
{
    return function;
}

llvm::Argument * FunctionGen::getArg(size_t i)
{
    assert(i < function->arg_size());
    auto it = function->arg_begin();
    std::advance(it, i);
    return &(*it);
}

void FunctionGen::setReturnValue(llvm::Value * value)
{
    returnValue = value;
}

CodeGen & FunctionGen::getCodeGen()
{
    return moduleGen.getCodeGen();
}

llvm::LLVMContext & FunctionGen::getContext()
{
    return moduleGen.getContext();
}

//-----------------------------------------------------------------------------

IfGen::IfGen(llvm::Value * condV) :
        functionGen(g_codeGen.getCurrentFunctionGen()),
        state(StateThen), mergeBB(nullptr)
{
    auto & context = functionGen.getContext();

    prevBB = g_codeGen->GetInsertBlock();

    // if condV evaluates to "true", the code in this block ist executed
    BasicBlock * thenBB = BasicBlock::Create(context, "then", functionGen.getFunction());
    blocks.push_back(thenBB);

    // can be either be used for "else" or for the following code
    BasicBlock * nextBB = BasicBlock::Create(context);
    blocks.push_back(nextBB);

    g_codeGen->CreateCondBr(condV, thenBB, nextBB);
    g_codeGen->SetInsertPoint(thenBB);
}

IfGen::IfGen(FunctionGen & functionGen, llvm::Value * condV) :
        functionGen(functionGen), state(StateThen), mergeBB(nullptr)
{
    auto & context = functionGen.getModuleGen().getContext();
    auto & codeGen = functionGen.getCodeGen();

    prevBB = codeGen->GetInsertBlock();

    // if condV evaluates to "true", the code in this block ist executed
    BasicBlock * thenBB = BasicBlock::Create(context, "then", functionGen.getFunction());
    blocks.push_back(thenBB);

    // can be either be used for "else" or for the following code
    BasicBlock * nextBB = BasicBlock::Create(context);
    blocks.push_back(nextBB);

    codeGen->CreateCondBr(condV, thenBB, nextBB);
    codeGen->SetInsertPoint(thenBB);
}

IfGen::IfGen(FunctionGen & functionGen, llvm::Value * condV, const IfGen::initial_values_vec_t & initialValues) :
        IfGen(functionGen, condV)
{
    // used to set up the PHI-Nodes
    for (auto & entry : initialValues) {
        incoming_vars_vec_t initial {{entry.second, prevBB}};
        ifVars.emplace_back(entry.first, std::move(initial));
    }
}

IfGen::~IfGen()
{
    if (state != StateDone) {
        EndIf();
    }
}

llvm::BasicBlock * IfGen::ElseIf(llvm::Value * condV)
{
    assert(state == StateThen || state == StateElseIf);

    auto & context = getContext();
    auto & codeGen = getCodeGen();

    // mergeBB is only needed if there is at least one "else" block
    if (mergeBB == nullptr) {
        mergeBB = BasicBlock::Create(context);
    }
    codeGen->CreateBr(mergeBB); // branch: previous -> mergeBB

    // create one basic block for the conditional branch
    BasicBlock * conditionTestBB = blocks.back();
    conditionTestBB->setName("else_if");
    functionGen.getFunction()->getBasicBlockList().push_back(conditionTestBB);
    codeGen->SetInsertPoint(conditionTestBB);

    // if condV evaluates to "true", the code in this block is executed
    BasicBlock * thenBB = BasicBlock::Create(context, "then");
    blocks.push_back(thenBB);

    // can be either be used for "else" or for the following code
    BasicBlock * nextBB = BasicBlock::Create(context);
    blocks.push_back(nextBB);

    codeGen->CreateCondBr(condV, thenBB, nextBB);

    functionGen.getFunction()->getBasicBlockList().push_back(thenBB);
    codeGen->SetInsertPoint(thenBB);

    state = StateElseIf;

    return nextBB;
}

llvm::BasicBlock * IfGen::Else()
{
    assert(state == StateThen || state == StateElseIf);

    auto & context = getContext();
    auto & codeGen = getCodeGen();

    if (mergeBB == nullptr) {
        mergeBB = BasicBlock::Create(context);
    }
    getCodeGen().leaveBlock(mergeBB);

    BasicBlock * currentBB = blocks.back();
    currentBB->setName("else");

    functionGen.getFunction()->getBasicBlockList().push_back(currentBB);
    codeGen->SetInsertPoint(currentBB);

    // prevent the creation of further ElseIf/Else blocks
    state = StateElse;

    return mergeBB;
}

void IfGen::EndIf()
{
    assert(state != StateDone);

    auto & codeGen = getCodeGen();

    if (mergeBB == nullptr) {
        mergeBB = blocks.back();
    }
    mergeBB->setName("end_if");
    getCodeGen().leaveBlock(mergeBB);

    // continue after the "if"-statement
    functionGen.getFunction()->getBasicBlockList().push_back(mergeBB);
    codeGen->SetInsertPoint(mergeBB);

    state = StateDone;
}

CodeGen & IfGen::getCodeGen()
{
    return functionGen.getCodeGen();
}

llvm::LLVMContext & IfGen::getContext()
{
    return functionGen.getContext();
}

llvm::Value * IfGen::getResult(size_t i)
{
    assert(state == StateDone);

    auto & codeGen = getCodeGen();

    auto & entry = ifVars[i];
    incoming_vars_vec_t & incomingVars = entry.second;

    // superfluous PHI-Node?
    if (incomingVars.size() < 2) {
        throw std::runtime_error("IfGen: expected at least one additional incoming Value.");
    }

    // now that all basic blocks are available it is time to set up the PHI-Nodes
    unsigned reservedValues = static_cast<unsigned>(incomingVars.size());
    llvm::PHINode * phiNode = codeGen->CreatePHI(
            incomingVars.front().first->getType(), reservedValues, entry.first);
    for (auto & incomingPair : incomingVars) {
        phiNode->addIncoming(incomingPair.first, incomingPair.second);
    }

    return phiNode;
}

void IfGen::setVar(size_t i, llvm::Value * value)
{
    assert(i < ifVars.size());
    llvm::BasicBlock * currentBB = getCodeGen()->GetInsertBlock();

    auto & entry = ifVars[i];
    incoming_vars_vec_t & in = entry.second;

    // ignore the default value of the PHI-Node
    if (state == StateElse) {
        in.erase( in.begin() );
    }

    in.emplace_back(value, currentBB);
}

//-----------------------------------------------------------------------------

LoopGen::LoopGen(FunctionGen & functionGen, const loop_var_vec_t & initialValues) :
        functionGen(functionGen)
{
    auto & context = getContext();
    auto & codeGen = getCodeGen();

    // set up the basic block structure
    BasicBlock * preheaderBB = codeGen->GetInsertBlock();
    loopBB = BasicBlock::Create(context, "loop", functionGen.getFunction());
    loopContBB = BasicBlock::Create(context, "loop_cont");
    afterBB = BasicBlock::Create(context, "loop_end");

    // enter the loop
    codeGen->CreateBr(loopBB);
    codeGen->SetInsertPoint(loopBB);

    // set up the PHI-Nodes
    for (auto & entry : initialValues) {
        llvm::PHINode * phiNode = codeGen->CreatePHI(entry.second->getType(), 2, entry.first);
        phiNode->addIncoming(entry.second, preheaderBB);
        loopVars.push_back(phiNode);
    }
}

LoopGen::LoopGen(FunctionGen & functionGen, llvm::Value * condV, const loop_var_vec_t & initialValues) :
    functionGen(functionGen)
{
    auto & context = getContext();
    auto & codeGen = getCodeGen();

    // set up the basic block structure
    BasicBlock * headerBB = BasicBlock::Create(context, "loop_header", functionGen.getFunction());
    loopBB = BasicBlock::Create(context, "loop", functionGen.getFunction());
    loopContBB = BasicBlock::Create(context, "loop_cont");
    afterBB = BasicBlock::Create(context, "loop_end");

    codeGen->CreateBr(headerBB);
    codeGen->SetInsertPoint(headerBB);

    // conditional entry
    codeGen->CreateCondBr(condV, loopBB, afterBB);
    codeGen->SetInsertPoint(loopBB);

    for (auto & entry : initialValues) {
        llvm::PHINode * phiNode = codeGen->CreatePHI(entry.second->getType(), 2, entry.first);
        phiNode->addIncoming(entry.second, headerBB);
        loopVars.push_back(phiNode);
    }
}

LoopGen::~LoopGen()
{ }

CodeGen & LoopGen::getCodeGen()
{
    return functionGen.getCodeGen();
}

void LoopGen::loopDone(llvm::Value * condV, const loop_val_vec_t & currentValues)
{
    auto & codeGen = getCodeGen();

    // add the new basic blocks
    Function * fun = functionGen.getFunction();
    fun->getBasicBlockList().push_back(loopContBB);
    fun->getBasicBlockList().push_back(afterBB);

    codeGen->SetInsertPoint(loopContBB);

    // conditional loop continuation
    codeGen->CreateCondBr(condV, loopBB, afterBB);
    codeGen->SetInsertPoint(afterBB);

    // sanity check
    if (loopVars.size() != currentValues.size()) {
        throw std::runtime_error("loopDone: loopVars.size() != currentValues.size()");
    }

    // finalize the PHI-Nodes
    for (size_t i = 0, length = currentValues.size(); i < length; ++i) {
        PHINode * phiNode = loopVars[i];
        phiNode->addIncoming(currentValues[i], loopContBB);
    }
}

void LoopGen::loopBreak()
{
    auto & codeGen = getCodeGen();
    codeGen.setNextBlock(afterBB);
}

void LoopGen::loopContinue()
{
    auto & codeGen = getCodeGen();
    codeGen.setNextBlock(loopContBB);
}

void LoopGen::loopBodyEnd()
{
    auto & codeGen = getCodeGen();
    codeGen.leaveBlock(loopContBB);
    codeGen->SetInsertPoint(loopContBB);
}

llvm::Value * LoopGen::getLoopVar(size_t i)
{
    assert(i < loopVars.size());
    return loopVars[i];
}

LLVMContext & LoopGen::getContext()
{
    return functionGen.getContext();
}

LoopBodyGen::LoopBodyGen(LoopGen & loopGen) :
    loopGen(loopGen)
{ }

LoopBodyGen::~LoopBodyGen()
{
    loopGen.loopBodyEnd();
}

//-----------------------------------------------------------------------------

namespace Functions {

Function * getPrintfPrototype(llvm::Module & module)
{
    auto & context = module.getContext();

    // lookup printf()
    llvm::FunctionType * type = llvm::TypeBuilder<int(char *, ...), false>::get(context);
    llvm::Function * func =
            cast<Function>(
                    module.getOrInsertFunction(
                            "printf", type,
                            llvm::AttributeList().addAttribute(context, 1u, llvm::Attribute::NoAlias)
                    )
            );

    return func;
}

Function * getPutcharPrototype(llvm::Module & module)
{
    auto & context = module.getContext();

    // lookup putchar()
    llvm::FunctionType * type = llvm::TypeBuilder<int(int), false>::get(context);
    llvm::Function * func = cast<Function>( module.getOrInsertFunction("putchar", type) );

    return func;
}

cg_int_t genPutcharCall(cg_int_t c)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & module = codeGen.getCurrentModuleGen().getModule();

    llvm::Function * func = getPutcharPrototype(module);
    llvm::CallInst * result = codeGen->CreateCall(func, c.getValue());

    return cg_int_t(result);
}

Function * getMemcmpPrototype(llvm::Module & module)
{
    auto & context = module.getContext();

    // lookup memcmp()
    llvm::FunctionType * type = llvm::TypeBuilder<int(void *, void *, size_t), false>::get(context);
    llvm::Function * func = cast<Function>( module.getOrInsertFunction("memcmp", type) );

    return func;
}

cg_int_t genMemcmpCall(cg_voidptr_t s1, cg_voidptr_t s2, cg_size_t n)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & module = codeGen.getCurrentModuleGen().getModule();

    llvm::Function * func = getMemcmpPrototype(module);
    llvm::CallInst * result = codeGen->CreateCall( func, {s1, s2, n} );

    return cg_int_t(result);
}

Function * getMallocPrototype(llvm::Module & module)
{
    auto & context = module.getContext();

    // lookup malloc()
    llvm::FunctionType * type = llvm::TypeBuilder<void *(size_t), false>::get(context);
    llvm::Function * func = cast<Function>( module.getOrInsertFunction("malloc", type) );

    return func;
}

cg_voidptr_t genMallocCall(cg_size_t size)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & module = codeGen.getCurrentModuleGen().getModule();

    llvm::Function * func = getMallocPrototype(module);
    llvm::CallInst * result = codeGen->CreateCall( func, {size} );

    return cg_voidptr_t(result);
}

Function * getFreePrototype(llvm::Module & module)
{
    auto & context = module.getContext();

    // lookup free()
    llvm::FunctionType * type = llvm::TypeBuilder<void(void *), false>::get(context);
    llvm::Function * func = cast<Function>( module.getOrInsertFunction("free", type) );

    return func;
}

void genFreeCall(cg_voidptr_t ptr)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & module = codeGen.getCurrentModuleGen().getModule();

    llvm::Function * func = getFreePrototype(module);
    codeGen->CreateCall( func, {ptr} );
}

} // end namespace Functions
