#include "specializer.h"
#include <unordered_map>
#include <unordered_set>
#include "hash.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;
using namespace llvm::orc;
using namespace std;

static unordered_set<string> symbols;
static unordered_map<uint64_t, intmap> func_counter;
static unordered_map<string, Function*> function_ir;

// Logs all symbols currently tracked by the specializer.
void LogSymbols(llvm::raw_ostream& io) {
    for (const string& s : symbols) io << s << "\n";
}

// Registers a symbol with the specializer as a function belonging to an
// active module. This means that calls to this function will be trampolined
// in the instrumentation pass.
void TrackSymbol(llvm::StringRef str) {
    symbols.insert(str);
}

// Defines a function for a particular name.
void DefineFunction(llvm::StringRef str, llvm::Function* fn) {
    function_ir[str] = fn;
}

static std::unordered_map<LLVMContext*, Function*> JIT_RESOLVE_DEFS;
static Function* JIT_RESOLVE_FN = nullptr;
static JITDylib* DYLIB = nullptr;
static SpecializationPass* SPECIALIZE = nullptr;

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
extern "C" JITTargetAddress JITResolveCall(JITTargetAddress fn, JITTargetAddress arg) {
    outs() << "Called function at address " << (void*)fn << " with specialized argument " << arg << "\n";

    intmap& curr_func = func_counter[fn];
    intmap::const_iterator curr_elm = curr_func.find(arg);
    
    uint64_t num_calls;
    if (curr_elm == curr_func.end()) {
        num_calls = 1;
    } else {
        // if optimized, run that instead
        if ((*curr_elm).second > SPECIALIZATION_THRESHOLD) {
            return (*curr_elm).second;
        }
        num_calls = (*curr_elm).second + 1;
    }

    // param used a lot, optimize it and use it
    if (num_calls >= SPECIALIZATION_THRESHOLD) {
        num_calls = CompileFunction(function_ir["fn"], arg);
        fn = num_calls;
    }
    
    curr_func.emplace(arg, num_calls);
    
    return fn;
}

// Adds JIT implementation functions to a module.
void DeclareInternalFunctions(llvm::LLVMContext& ctx, Module* module) {
    if (!JIT_RESOLVE_FN) JIT_RESOLVE_FN = Function::Create(
        FunctionType::get(Type::getInt64Ty(ctx), { Type::getInt64Ty(ctx), Type::getInt64Ty(ctx) }, false), 
        Function::ExternalLinkage, 
        "JITResolveCall", 
        module
    );
}

// Adds JIT implementation functions to dynamic linker.
void AddInternalFunctions(MangleAndInterner& mangle, SymbolMap& map) {
    map[mangle("JITResolveCall")] = JITEvaluatedSymbol(pointerToJITTargetAddress(&JITResolveCall), {});
}

int findSpecializedArg(Function* fn) {
    FunctionType* type = fn->getFunctionType();
    int i = 0;
    for (Type* argt : type->params()) {
        if (isa<IntegerType>(argt) && ((IntegerType*)argt)->getScalarSizeInBits() <= 64) // less than or equal to word size
            return i;
        i ++;
    }
    return -1;
}

// static std::unique_ptr<legacy::FunctionPassManager> FPM = nullptr;

// class SpecializingMaterializationUnit : public MaterializationUnit {
// public:

// };

// 1. Create function pass manager if absent.
// 2. Set argument parameter in specialization pass.
// 3. Clone function, change name to specialized one. (?)
// 4. Run specializing FPM over cloned function.
// 5. Materialize the function. (how?)
// 6. Lookup the specialized function name in the execution session, return address.

// SpecializationPass* SPECIALIZE = nullptr;

void InitSpecializer(ThreadSafeModule& module, JITDylib* dylib, IRTransformLayer* tl, ThreadSafeContext ctx) {
    DYLIB = dylib;
    auto fn_module = CloneModule(*module.getModuleUnlocked());
    fn_module->setModuleIdentifier(module.getModuleUnlocked()->getModuleIdentifier() + "_specialized");
    outs() << "original module:\n";
    module.getModuleUnlocked()->print(outs(), nullptr);
    outs() << "copied module:\n";
    fn_module->print(outs(), nullptr);
    auto tsm = ThreadSafeModule(std::move(fn_module), ctx);
    SPECIALIZE = new SpecializationPass();
    Error e = tl->add(*DYLIB, move(tsm));
    if (e) {
        errs() << "Error adding specializer module to transform layer.\n";
    }
}

// class SpecializedMaterializationUnit : public MaterializationUnit {
//     static SymbolFlagsMap getSymbolMap(const std::string& name) {
//         SymbolFlagsMap map;
//         map.insert({ DYLIB->getExecutionSession().getSymbolStringPool()->intern(name), JITSymbolFlags()});
//         return map;
//     }
// public:
//     SpecializedMaterializationUnit(const std::string& name, IRCompileLayer& CL): 
//         MaterializationUnit(getSymbolMap(name), 0), CompileLayer(CL) {}

//     StringRef getName() const override {
//         return "SpecializedMaterializationUnit";
//     }

//     void materialize(MaterializationResponsibility R) override {
//         CompileLayer.emit(std::move(R), ThreadSafeModule(;
//     }
// private:
//     void discard(const JITDylib &JD, const SymbolStringPtr &Sym) override {
//         llvm_unreachable("Specialized functions are not overridable");
//     }

//     IRCompileLayer& CompileLayer;
// };
llvm::Expected<llvm::orc::ThreadSafeModule> specializeModule(llvm::orc::ThreadSafeModule M, const llvm::orc::MaterializationResponsibility &R) {
    // Create a function pass manager.
    auto FPM = std::make_unique<legacy::FunctionPassManager>(M.getModuleUnlocked());

    // Add some optimizations.
    FPM->add(SPECIALIZE);
    FPM->doInitialization();

    // Run the optimizations over all functions in the module being added to
    // the JIT.
    for (auto &F : *M.getModuleUnlocked())
      FPM->run(F);

    return M;
}

// Compiles a function specialized on a particular input.
JITTargetAddress CompileFunction(Function* function, JITTargetAddress arg) {
    std::string mangled = function->getName().str() + "(" + to_string((uint64_t)arg) + ")";
    
    std::vector<Type*> argts;
    for (const Argument &I : function->args())
      argts.push_back(I.getType());
    FunctionType *fty = FunctionType::get(function->getFunctionType()->getReturnType(),
                                      argts, function->getFunctionType()->isVarArg());
    Function* copy = Function::Create(fty, Function::ExternalLinkage, mangled);
    ValueToValueMapTy vmap;
    Function::arg_iterator DestI = copy->arg_begin();
    for (const Argument & I : function->args())
        if (vmap.count(&I) == 0) {     // Is this argument preserved?
            DestI->setName(I.getName()); // Copy the name over...
            vmap[&I] = &*DestI++;        // Add mapping to VMap
        }
    SmallVector<ReturnInst*, 8> returns;
    CloneFunctionInto(copy, function, vmap, true, returns);

    SPECIALIZE->setValue(arg);
    LLVMContext& ctx = copy->getContext();

    // outs() << "Specialized function " << function->getName() << " for argument " << arg << "\n";
    // copy->print(outs());

    ExecutionSession& ES = DYLIB->getExecutionSession();
    auto sym = ES.lookup({DYLIB}, mangled);
    if (!sym) {
        errs() << "Failed to specialize function " << function->getName() << " for argument " << arg << "\n";
        return 0;
    }
    return sym->getAddress();
}

// Specializes the provided function on a particular argument.
SpecializationPass::SpecializationPass(): FunctionPass(pid) {}

void SpecializationPass::setValue(llvm::JITTargetAddress arg_in) {
    arg = arg_in;
}

bool SpecializationPass::runOnFunction(Function &f) {
    if (f.arg_begin() == f.arg_end()) return true;

    Value* fnarg = nullptr;
    fnarg = dyn_cast<Value>(f.arg_begin());
    ConstantInt* const_val = llvm::ConstantInt::get(f.getContext(), llvm::APInt(/*nbits*/32, arg, /*bool*/false));
    outs() << "Constant val: " << *const_val << "\n";
    fnarg->replaceAllUsesWith(const_val);
    return true;
}

// Inserts trampolines into functions. Transforms all function calls to active module functions
// into indirect calls, using the JITResolveCall function to resolve the address prior to invocation.
InstrumentationPass::InstrumentationPass(): FunctionPass(pid) {}

bool InstrumentationPass::runOnFunction(Function &f) {
    LLVMContext& ctx = f.getContext();
    for (auto& bb : f) {
        for (auto& inst : bb) {
            if (isa<CallInst>(&inst)) {
                CallInst& call = (CallInst&)inst;
                int argidx;
                if (call.getCalledFunction() 
                    && symbols.find(call.getCalledFunction()->getName()) != symbols.end()
                    && (argidx = findSpecializedArg(call.getCalledFunction())) > -1) {
                    FunctionType* fnt = call.getCalledFunction()->getFunctionType();
                    Instruction* orig = BitCastInst::Create(Instruction::CastOps::SExt, call.getCalledFunction(), Type::getInt64Ty(ctx));
                    Instruction* arg = BitCastInst::Create(Instruction::CastOps::SExt, call.getArgOperand(argidx), Type::getInt64Ty(ctx));
                    Instruction* rawchosen = CallInst::Create(JIT_RESOLVE_FN->getFunctionType(), JIT_RESOLVE_FN, { orig, arg }, "");
                    Instruction* chosen = BitCastInst::Create(Instruction::CastOps::BitCast, rawchosen, PointerType::get(fnt, 0));
                    orig->insertBefore(&call);
                    arg->insertBefore(&call);
                    rawchosen->insertBefore(&call);
                    chosen->insertBefore(&call);
                    call.setCalledFunction(fnt, chosen);
                }
            }
        }
    }
    return true; // TODO
}