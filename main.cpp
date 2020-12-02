#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/TargetProcessControl.h"
#include "llvm/ExecutionEngine/Orc/TPCIndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Constants.h>

namespace llvm {

class HelloWorldPass : public FunctionPass {
  char pid = 74;
public:
  HelloWorldPass(): FunctionPass(pid) {}

  bool runOnFunction(Function &f) override {
    for (BasicBlock& bb : f) {
      for (Instruction& i : bb) {
        for (int j = 0; j < i.getNumOperands(); j ++) {
          Value* value = i.getOperand(j);
          if (isa<ConstantInt>(value)) i.setOperand(j, ConstantInt::getSigned(IntegerType::get(f.getContext(), 32), 9));
        }
      }
    }
    return true;
  }
};

namespace orc {

class JIT {
private:
  std::unique_ptr<ExecutionSession> ES;

  std::unique_ptr<TargetProcessControl> TPC;
  std::unique_ptr<TPCIndirectionUtils> TPCIU;

  DataLayout DL;
  MangleAndInterner Mangle;

  RTDyldObjectLinkingLayer ObjectLayer;
  IRCompileLayer CompileLayer;
  IRTransformLayer TransformLayer;
  CompileOnDemandLayer CODLayer;
  ThreadSafeContext Ctx;

  JITDylib& MainJD;

  static Expected<ThreadSafeModule> optimizeModule(ThreadSafeModule M, const MaterializationResponsibility &R) {
    // Create a function pass manager.
    auto FPM = std::make_unique<legacy::FunctionPassManager>(M.getModuleUnlocked());

    // Add some optimizations.
    FPM->add(new HelloWorldPass());
    FPM->doInitialization();

    // Run the optimizations over all functions in the module being added to
    // the JIT.
    for (auto &F : *M.getModuleUnlocked())
      FPM->run(F);

    return M;
  }
  
  static void handleLazyCallThroughError() {
    errs() << "LazyCallThrough error: Could not find function body";
    exit(1);
  }

public:
  JIT(std::unique_ptr<ExecutionSession> ES, std::unique_ptr<TargetProcessControl> TPC, std::unique_ptr<TPCIndirectionUtils> TPCIU, JITTargetMachineBuilder JTMB, DataLayout DL)
      : ES(std::move(ES),
        ObjectLayer(ES,
          []() { return std::make_unique<SectionMemoryManager>(); }),
        CompileLayer(ES, ObjectLayer, std::make_unique<ConcurrentIRCompiler>(std::move(JTMB))),
        TransformLayer(ES, CompileLayer, optimizeModule),
        TPC(std::move(TPC)), TPCIU(std::move(TPCIU)),
        DL(std::move(DL)), Mangle(ES, this->DL),
        Ctx(std::make_unique<LLVMContext>()),
        CODLayer(*this->ES, OptimizeLayer,
                 this->TPCIU->getLazyCallThroughManager(),
                 [this] { return this->TPCIU->createIndirectStubsManager(); }),
        MainJD(ES.createJITDylib("main")) {
    MainJD.addGenerator(
        cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess('_')));
    // SymbolMap syms;
    // syms[Mangle("puts")] = JITEvaluatedSymbol(
    //     pointerToJITTargetAddress(&puts), JITSymbolFlags());
    // MainJD.define(absoluteSymbols(syms));
  }

  ~JIT() {
    // if (auto Err = ES.releaseVModule())
    //   ES->reportError(std::move(Err));
  }

  static Expected<std::unique_ptr<JIT>> Create() {
    auto SSP = std::make_shared<SymbolStringPool>();
    auto TPC = SelfTargetProcessControl::Create(SSP);
    if (!TPC)
      return TPC.takeError();
    auto ES = std::make_unique<ExecutionSession>(SSP);
    auto TPCIU = TPCIndirectionUtils::Create(**TPC);
    if (!TPCIU)
      return TPCIU.takeError();
    
    (*TPCIU)->createLazyCallThroughManager(
        *ES, pointerToJITTargetAddress(&handleLazyCallThroughError));

    if (auto Err = setUpInProcessLCTMReentryViaTPCIU(**TPCIU))
      return std::move(Err);

    JITTargetMachineBuilder JTMB((*TPC)->getTargetTriple());
    if (!JTMB)
      return JTMB.takeError();

    auto DL = JTMB->getDefaultDataLayoutForTarget();
    if (!DL)
      return DL.takeError();

    return std::make_unique<JIT>(std::move(ES), std::move(TPC), std::move(TPCIU), std::move(*JTMB), std::move(*DL));
  }

  // void addLibrary(const char* fileName) {
  //   MainJD.addGenerator(cantFail(
  //     DynamicLibrarySearchGenerator::Load(fileName, '_', [](auto sym) -> bool { return true; })));
  // }

  const DataLayout &getDataLayout() const { return DL; }

  Error addModule(ThreadSafeModule&& TSM) {
    return TransformLayer.add(MainJD, std::move(TSM));
  }

  Expected<JITEvaluatedSymbol> lookup(StringRef Name) {
    return ES.lookup({&MainJD}, Mangle(Name.str()));
  }
};

} // end namespace orc
} // end namespace llvm

using namespace llvm::orc;
using namespace llvm;

int main(int argc, char** argv) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext>();
    SMDiagnostic error;
    auto module = parseIRFile(argv[1], error, *context);
    auto tsm = std::make_unique<ThreadSafeModule>(move(module), move(context));

    auto optionaljit = JIT::Create();
    if (!optionaljit)
      outs() << optionaljit.takeError() << "\n";
    auto jit = move(optionaljit.get());
    jit->addModule(std::move(*tsm));
    auto* main = (int(*)(int, char*[]))jit->lookup("main").get().getAddress();

    char args[] = "<main>";
    char* ptr = args;
    return main(1, &ptr);
}