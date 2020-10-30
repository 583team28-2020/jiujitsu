#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
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
  ExecutionSession ES;

  DataLayout DL;
  MangleAndInterner Mangle;

  RTDyldObjectLinkingLayer ObjectLayer;
  IRCompileLayer CompileLayer;
  IRTransformLayer TransformLayer;
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

public:
  JIT(JITTargetMachineBuilder JTMB, DataLayout DL)
      : ObjectLayer(ES,
                    []() { return std::make_unique<SectionMemoryManager>(); }),
        CompileLayer(ES, ObjectLayer, std::make_unique<ConcurrentIRCompiler>(std::move(JTMB))),
        TransformLayer(ES, CompileLayer, optimizeModule),
        DL(std::move(DL)), Mangle(ES, this->DL),
        Ctx(std::make_unique<LLVMContext>()),
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
    auto ES = std::make_unique<ExecutionSession>();

    auto JTMB = JITTargetMachineBuilder::detectHost();
    if (!JTMB)
      return JTMB.takeError();

    auto DL = JTMB->getDefaultDataLayoutForTarget();
    if (!DL)
      return DL.takeError();

    return std::make_unique<JIT>(std::move(*JTMB), std::move(*DL));
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