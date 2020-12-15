#include "specializer.h"
#include <unordered_map>
#include <unordered_set>
#include "hash.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Scalar/GVN.h"

using namespace llvm;
using namespace llvm::orc;
using namespace std;

static unordered_set<string> symbols;
static unordered_map<uint64_t, intmap> func_counter;
static unordered_map<string, Function*> function_ir;
static unordered_set<string> debug_flags;

void AddDebugFlag(StringRef str) {
    debug_flags.insert(str.str());
}

bool IsDebugFlag(StringRef str) {
    return debug_flags.find(str.str()) != debug_flags.end();
}

// Logs all symbols currently tracked by the specializer.
void LogSymbols(llvm::raw_ostream& io) {
    for (const string& s : symbols) io << s << "\n";
}

// Registers a symbol with the specializer as a function belonging to an
// active module. This means that calls to this function will be trampolined
// in the instrumentation pass.
void TrackSymbol(llvm::StringRef str) {
    symbols.insert(str.str());
}

// Defines a function for a particular name.
void DefineFunction(llvm::StringRef str, llvm::Function* fn) {
    function_ir[str.str()] = fn;
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
extern "C" JITTargetAddress JITResolveCall(JITTargetAddress fn, JITTargetAddress arg, const char* name) {
    // outs() << "Called function at address " << (void*)fn << " with specialized argument " << arg << "\n";

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
        auto it = function_ir.find(name);
        if (it != function_ir.end()) {
            num_calls = fn = CompileFunction(function_ir[name], arg);
            if (!fn) {
                DYLIB->dump(errs());
                errs() << "Failed to compile function!\n";
            }
        }
    }
    
    curr_func.emplace(arg, num_calls);
    
    return fn;
}

// Adds JIT implementation functions to a module.
void DeclareInternalFunctions(llvm::LLVMContext& ctx, Module* module) {
    Function::Create(
        FunctionType::get(Type::getInt64Ty(ctx), { Type::getInt64Ty(ctx), Type::getInt64Ty(ctx), Type::getInt8PtrTy(ctx) }, false), 
        Function::ExternalLinkage, 
        "JITResolveCall", 
        module
    );
}

static MangleAndInterner* MANGLE = nullptr;

// Adds JIT implementation functions to dynamic linker.
void AddInternalFunctions(MangleAndInterner& mangle, SymbolMap& map) {
    MANGLE = &mangle;
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

static IRTransformLayer* SPECIALIZE_TRANSFORM = nullptr;
static ThreadSafeContext CTX;

void InitSpecializer(JITDylib* dylib, IRTransformLayer* tl, ThreadSafeContext ctx) {
    DYLIB = dylib;
    CTX = ctx;
    SPECIALIZE = new SpecializationPass();
    SPECIALIZE_TRANSFORM = tl;
}

llvm::Expected<llvm::orc::ThreadSafeModule> specializeModule(llvm::orc::ThreadSafeModule M, const llvm::orc::MaterializationResponsibility &R) {
    auto FPM = std::make_unique<legacy::FunctionPassManager>(M.getModuleUnlocked());
    FPM->add(new SpecializationPass());
    FPM->add(createInstructionCombiningPass());
    FPM->add(createReassociatePass());
    FPM->add(createGVNPass());
    FPM->add(createCFGSimplificationPass());
    FPM->add(createPromoteMemoryToRegisterPass());
    FPM->add(createConstantPropagationPass());
    FPM->add(createDeadCodeEliminationPass());
    FPM->doInitialization();
    for (auto &F : *M.getModuleUnlocked()) {
      FPM->run(F);
      F.print(outs());
    }

    return M;
}

class SpecializationMaterializer : public MaterializationUnit {
    ThreadSafeModule tsm;
    JITTargetAddress arg;
    std::string name;

    static SymbolFlagsMap getSymbolMap(SymbolStringPtr name) {
        SymbolFlagsMap map;
        map.insert({name, JITSymbolFlags::Exported | JITSymbolFlags::Callable});
        return map;
    }
public:
    SpecializationMaterializer(SymbolStringPtr sym, ThreadSafeModule&& tsm_in, JITTargetAddress arg_in): 
        MaterializationUnit(getSymbolMap(sym), sym, 0), tsm(move(tsm_in)), arg(arg_in) {
        name = "Materializer_" + std::string(*sym);
    } 
    
    StringRef getName() const override {
        return name;
    }
protected:
    void materialize(MaterializationResponsibility R) override {
        SPECIALIZE->setValue(arg);
        SPECIALIZE_TRANSFORM->emit(std::move(R), std::move(tsm));
    }
    
    void discard(const JITDylib &JD, const SymbolStringPtr &Name) override {
        llvm_unreachable("Should not discard specialized functions.");
    }
};

// Compiles a function specialized on a particular input.
JITTargetAddress CompileFunction(Function* function, JITTargetAddress arg) {
    std::string mangled = function->getName().str() + "_" + to_string((uint64_t)arg);
    ThreadSafeModule tsm(std::make_unique<Module>(mangled, *CTX.getContext()), CTX);
    DeclareInternalFunctions(*tsm.getContext().getContext(), tsm.getModuleUnlocked());
    
    std::vector<Type*> argts;
    for (const Argument &I : function->args())
      argts.push_back(I.getType());
    FunctionType *fty = FunctionType::get(function->getFunctionType()->getReturnType(),
                                      argts, function->getFunctionType()->isVarArg());
    Function* copy = Function::Create(fty, Function::ExternalLinkage, mangled, tsm.getModuleUnlocked());
    ValueToValueMapTy vmap;
    Function::arg_iterator DestI = copy->arg_begin();
    for (const Argument & I : function->args())
        if (vmap.count(&I) == 0) {     
            DestI->setName(I.getName()); 
            vmap[&I] = &*DestI++;      
        }
    SmallVector<ReturnInst*, 8> returns;
    CloneFunctionInto(copy, function, vmap, true, returns);

    ExecutionSession& ES = DYLIB->getExecutionSession();
    auto def = DYLIB->define(std::make_unique<SpecializationMaterializer>((*MANGLE)(mangled), std::move(tsm), arg));
    if (def) {
        errs() << "Failed to define specialized function in dylib.\n";
        return 0;
    }

    auto sym = ES.lookup({DYLIB}, (*MANGLE)(mangled));

    if (IsDebugFlag("-dumpjd")) {
        outs() << "Dumping JITDylib contents\n";
        DYLIB->dump(outs());
        outs() << "\n";
    }

    sym = ES.lookup({DYLIB}, (*MANGLE)(mangled));
    if (!sym) {
        errs() << "Failed to specialize function " << function->getName() << " for argument " << arg << "\n";
        return 0;
    }
    return sym->getAddress();
}

// Specializes the provided function on a particular argument.
JITTargetAddress SpecializationPass::arg;

SpecializationPass::SpecializationPass(): FunctionPass(pid) {}

void SpecializationPass::setValue(llvm::JITTargetAddress arg_in) {
    arg = arg_in;
}

bool SpecializationPass::runOnFunction(Function &f) {
    if (f.arg_begin() == f.arg_end()) return true;

    Value* fnarg = nullptr;
    fnarg = dyn_cast<Value>(f.arg_begin());
    ConstantInt* const_val = llvm::ConstantInt::get(f.getContext(), llvm::APInt(fnarg->getType()->getScalarSizeInBits(), arg, false));
    fnarg->replaceAllUsesWith(const_val);

    if (IsDebugFlag("-log-spec")) {
        outs() << "Specialized function " << f.getName() << " on argument " << arg << "\n";
        f.print(outs());
        outs() << "\n";
    }
    return true;
}

// Inserts trampolines into functions. Transforms all function calls to active module functions
// into indirect calls, using the JITResolveCall function to resolve the address prior to invocation.
InstrumentationPass::InstrumentationPass(): FunctionPass(pid) {}

bool InstrumentationPass::doInitialization(Module &m) {
    resolveFn = m.getFunction("JITResolveCall");
    return resolveFn;
}

bool InstrumentationPass::runOnFunction(Function &f) {
    LLVMContext& ctx = f.getContext();
    for (auto& bb : f) {
        for (auto& inst : bb) {
            if (isa<CallInst>(&inst)) {
                CallInst& call = (CallInst&)inst;
                int argidx;
                if (call.getCalledFunction() 
                    && symbols.find(call.getCalledFunction()->getName().str()) != symbols.end()
                    && (argidx = findSpecializedArg(call.getCalledFunction())) > -1) {
                    FunctionType* fnt = call.getCalledFunction()->getFunctionType();
                    auto it = symbols.find(call.getCalledFunction()->getName().str());
                    Constant* nameConst = ConstantInt::get(Type::getInt64Ty(ctx), APInt(64, (uint64_t)it->c_str()));
                    Instruction* str = BitCastInst::Create(Instruction::CastOps::BitCast, nameConst, Type::getInt8PtrTy(ctx));
                    Instruction* orig = BitCastInst::Create(Instruction::CastOps::SExt, call.getCalledFunction(), Type::getInt64Ty(ctx));
                    Instruction* arg = BitCastInst::Create(Instruction::CastOps::SExt, call.getArgOperand(argidx), Type::getInt64Ty(ctx));
                    Instruction* rawchosen = CallInst::Create(resolveFn->getFunctionType(), resolveFn, { orig, arg, str });
                    Instruction* chosen = BitCastInst::Create(Instruction::CastOps::BitCast, rawchosen, PointerType::get(fnt, 0));
                    str->insertBefore(&call);
                    orig->insertBefore(&call);
                    arg->insertBefore(&call);
                    rawchosen->insertBefore(&call);
                    chosen->insertBefore(&call);
                    call.setCalledFunction(fnt, chosen);
                }
            }
        }
    }
    if (IsDebugFlag("-log-inst")) {
        outs() << "Added instrumentation to function " << f.getName() << "\n";
        f.print(outs());
        outs() << "\n";
    }
    return true;
}