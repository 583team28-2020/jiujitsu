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
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "specializer.h"


static llvm::orc::ThreadSafeContext TSC;

namespace llvm {

class PrintVisitorPass : public FunctionPass {
  char pid = 75;
public:
  PrintVisitorPass(): FunctionPass(pid) {}

  bool runOnFunction(Function& f) override {
    int i = 0;
    outs() << "Compiling function: " << f.getName() << "\n";
    for (auto& arg : f.args()) {
      outs() << " - Arg " << arg.getArgNo() << " : ";
      arg.getType()->print(outs());
      outs() << "\n";
    }
    f.print(outs());
    return true;
  }
};

namespace orc {

class CustomObjectLayer : public RTDyldObjectLinkingLayer {
  static const object::ObjectFile* objptr;

  static void onLoaded(VModuleKey, const object::ObjectFile& obj,
                         const RuntimeDyld::LoadedObjectInfo& info) {
    if (IsDebugFlag("-dbgloads")) {
      objptr = &obj;
      for (const auto& sym : obj.symbols()) {
        auto sec = cantFail(sym.getSection());
        if (sec != obj.section_end()) {
          uint64_t sectionAddr = info.getSectionLoadAddress(*sec);
          outs() << " - loaded " << cantFail(sym.getName()) << " : " << (void*)(sectionAddr + cantFail(sym.getValue())) << "\n";
        }
      }
    }
  }

public:
  CustomObjectLayer(ExecutionSession& ES, GetMemoryManagerFunction fn):
    RTDyldObjectLinkingLayer(ES, fn) {
    setNotifyLoaded(onLoaded);
  }
};

const object::ObjectFile* CustomObjectLayer::objptr = nullptr;

class JIT {
private:
  std::unique_ptr<ExecutionSession> ES;
  std::unique_ptr<LazyCallThroughManager> LCM;

  Triple triple;
  DataLayout DL;
  MangleAndInterner Mangle;

  CustomObjectLayer ObjectLayer;
  IRCompileLayer CompileLayer, SpecializeCompileLayer;
  IRTransformLayer TransformLayer, SpecializeTransformLayer;
  CompileOnDemandLayer CODLayer;
  ThreadSafeContext Ctx;

  JITDylib& MainJD;

  static Expected<ThreadSafeModule> optimizeModule(ThreadSafeModule M, const MaterializationResponsibility &R) {
    // Create a function pass manager.
    auto FPM = std::make_unique<legacy::FunctionPassManager>(M.getModuleUnlocked());

    // Add some optimizations.
    if (!IsDebugFlag("-no-inst")) FPM->add(new InstrumentationPass());
    FPM->add(createCFGSimplificationPass());
    FPM->add(createPromoteMemoryToRegisterPass());
    FPM->add(createGVNPass());
    FPM->add(createReassociatePass());
    FPM->add(createConstantPropagationPass());
    FPM->add(createInstructionCombiningPass());
    FPM->add(createDeadCodeEliminationPass());
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
  JIT(std::unique_ptr<ExecutionSession> ES, JITTargetMachineBuilder JTMB, DataLayout DL, const Triple& T, std::unique_ptr<LazyCallThroughManager>&& lcm)
      : ES(std::move(ES)),
        ObjectLayer(*this->ES,
          []() { return std::make_unique<SectionMemoryManager>(); }),
        CompileLayer(*this->ES, ObjectLayer, std::make_unique<ConcurrentIRCompiler>(JTMB)),
        TransformLayer(*this->ES, CompileLayer, optimizeModule),
        SpecializeCompileLayer(*this->ES, ObjectLayer, std::make_unique<ConcurrentIRCompiler>(JTMB)),
        SpecializeTransformLayer(*this->ES, SpecializeCompileLayer, specializeModule),
        DL(std::move(DL)), Mangle(*this->ES, this->DL),
        triple(T),
        LCM(std::move(lcm)),
        Ctx(std::make_unique<LLVMContext>()),
        CODLayer(*this->ES, TransformLayer, *LCM, createLocalIndirectStubsManagerBuilder(triple)),
        MainJD(cantFail(this->ES->createJITDylib("main"))) {
    MainJD.addGenerator(
        cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess('_')));
    SymbolMap syms;
    AddInternalFunctions(Mangle, syms);
    cantFail(MainJD.define(absoluteSymbols(syms)));
    CODLayer.setPartitionFunction(CompileOnDemandLayer::compileRequested); // Compile functions individually, only when they are needed.
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
    auto ES = std::make_unique<ExecutionSession>(std::move(SSP));

    auto JTMB = JITTargetMachineBuilder::detectHost();
    if (!JTMB)
      return JTMB.takeError();

    auto DL = JTMB->getDefaultDataLayoutForTarget();
    if (!DL)
      return DL.takeError();

    auto LCM = createLocalLazyCallThroughManager(JTMB->getTargetTriple(), *ES, pointerToJITTargetAddress(handleLazyCallThroughError));
    if (!LCM)
      return LCM.takeError();

    return std::make_unique<JIT>(std::move(ES), std::move(*JTMB), std::move(*DL), JTMB->getTargetTriple(), std::move(*LCM));
  }

  void addLibrary(const char* fileName) {
    MainJD.addGenerator(cantFail(
      DynamicLibrarySearchGenerator::Load(fileName, DL.getGlobalPrefix())));
  }

  const DataLayout &getDataLayout() const { return DL; }

  Error addModule(ThreadSafeModule&& TSM) {
    InitSpecializer(&MainJD, &SpecializeTransformLayer, TSC);
    return CODLayer.add(MainJD, std::move(TSM));
  }

  Expected<JITEvaluatedSymbol> lookup(StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }
};

} // end namespace orc
} // end namespace llvm

using namespace llvm::orc;
using namespace llvm;

static std::unordered_set<std::string> valid_flags = {
  "-log-inst", // log instrumented IR
  "-log-spec", // log specialized IR
  "-dumpjd", // dump jitdylib contents after compilation
  "-dbgloads", // log output when symbols are loaded into an object
  "-no-inst", // disable instrumentation
  "-no-spec", // disable specialization
};

void printUsage() {
  outs() << "Usage: ./jiujitsu <bitcode file> [flags...]\n";
  outs() << " -log-inst : Log instrumented IR.\n";
  outs() << " -log-spec : Log specialized IR.\n";
  outs() << " -dumpjd : Dump JITDylib after compiling a specialized function.\n";
  outs() << " -dbgloads : Log output when symbols are loaded.\n";
  outs() << " -no-inst : Disable instrumentation. Effectively disables specialization.\n";
  outs() << " -no-spec : Disable specialization. Still incurs profiling overhead.\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
      printUsage();
      return 1;
    }

    for (int i = 2; i < argc; i ++) {
      if (valid_flags.find(argv[i]) != valid_flags.end()) AddDebugFlag(argv[i]);
      else {
        printUsage();
        return 1;
      }
    }
    // ::llvm::DebugFlag = true;
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    TSC = ThreadSafeContext(std::move(std::make_unique<LLVMContext>()));
    SMDiagnostic error;
    auto module = parseIRFile(argv[1], error, *TSC.getContext());
    auto src_module = parseIRFile(argv[1], error, *TSC.getContext());
    DeclareInternalFunctions(*TSC.getContext(), module.get());
    DeclareInternalFunctions(*TSC.getContext(), src_module.get());
    auto tsm = std::make_unique<ThreadSafeModule>(move(module), TSC);
    
    for (auto& fn : src_module->getFunctionList()) {
      TrackSymbol(fn.getName());
      DefineFunction(fn.getName(), &fn);
    }
    SetSourceModule({ move(src_module), TSC });

    auto optionaljit = JIT::Create();
    if (!optionaljit)
      outs() << optionaljit.takeError() << "\n";
    auto jit = move(optionaljit.get());
    jit->addLibrary("/usr/lib/x86_64-linux-gnu/libc.so.6");
    if (Error e = jit->addModule(std::move(*tsm))) {
      errs() << "Error adding module.\n";
      return 1;
    }
    auto* main = (int(*)(int, char*[]))jit->lookup("main").get().getAddress();

    char args[] = "<main>";
    char* ptr = args;
    return main(1, &ptr);
}