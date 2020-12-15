#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/OrcABISupport.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Constants.h>

#define SPECIALIZATION_THRESHOLD 100LU

void AddDebugFlag(llvm::StringRef str);
bool IsDebugFlag(llvm::StringRef str);

// Logs all symbols currently tracked by the specializer.
void LogSymbols(llvm::raw_ostream& io);

// Registers a symbol with the specializer as a function belonging to an
// active module. This means that calls to this function will be trampolined
// in the instrumentation pass.
void TrackSymbol(llvm::StringRef str);

// Defines a function for a particular name.
void DefineFunction(llvm::StringRef str, llvm::Function* fn);

llvm::Expected<llvm::orc::ThreadSafeModule> specializeModule(llvm::orc::ThreadSafeModule M, const llvm::orc::MaterializationResponsibility &R);

// Returns the address of the function specialized for the given argument. Has three effects:
//  1. If the function is specialized on the argument, the count will be an address, numerically
//     greater than the specialization threshold. Return this address and do not modify the count.
//  2. If the function is not specialized, but is about to cross the threshold, we specialize the
//     function with our optimization passes on the input and set the count to the new function's address.
//  3. If the function is not specialized, and the count will not exceed the threshold, increment the
//     count and return the normal function address.
// Note that by reusing the count to store the specialized function pointer, we lose the ability to
// profile functions that are already specialized. However, this allows us to implement all of our
// lookup tables as simple int-to-int maps, which permits for optimization.
extern "C" llvm::JITTargetAddress JITResolveCall(llvm::JITTargetAddress fn, llvm::JITTargetAddress arg, const char* name);

// Adds JIT implementation functions to a module.
void DeclareInternalFunctions(llvm::LLVMContext& ctx, llvm::Module* module);

// Sets global source module. This contains LLVM IR before any optimizations are applied.
void SetSourceModule(llvm::orc::ThreadSafeModule&& tsm);

// Adds JIT implementation functions to dynamic linker.
void AddInternalFunctions(llvm::orc::MangleAndInterner& mangle, llvm::orc::SymbolMap& map);

// Initializes specializer with module and other info.
void InitSpecializer(llvm::orc::JITDylib* dylib, llvm::orc::IRTransformLayer* cl, llvm::orc::ThreadSafeContext ctx);

// Compiles a function specialized on a particular input.
llvm::JITTargetAddress CompileFunction(llvm::Function* function, llvm::JITTargetAddress arg);

// Specializes the provided function on a particular argument.
class SpecializationPass : public llvm::FunctionPass {
  char pid = 74;
  static llvm::JITTargetAddress arg;
public:
  SpecializationPass();
  static void setValue(llvm::JITTargetAddress arg_in);
  bool runOnFunction(llvm::Function &f) override;
};

// Inserts trampolines into functions. Transforms all function calls to active module functions
// into indirect calls, using the JITResolveCall function to resolve the address prior to invocation.
class InstrumentationPass : public llvm::FunctionPass {
    char pid = 76;
    llvm::Function* resolveFn = nullptr;
public:
    InstrumentationPass();
    bool doInitialization(llvm::Module &f) override;
    bool runOnFunction(llvm::Function &f) override;
};