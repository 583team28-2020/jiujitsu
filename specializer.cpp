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
    
    // add function if not being tracked
    if (func_counter.find(fn) == func_counter.end()) {
        func_counter[fn] = intmap();
    }

    intmap& curr_func = func_counter[fn];
    intmap::const_iterator curr_elm = curr_func.find(arg);
    
    // if optimized, run that instead
    if ((*curr_elm).second > SPECIALIZATION_THRESHOLD) {
        return (*curr_elm).second;
    }

    uint64_t num_calls;
    if (curr_elm == curr_func.end()) {
        num_calls = 1;
    } else {
        num_calls = (*curr_elm).second + 1;
    }

    // param used a lot, optimize it and use it
    if (num_calls == SPECIALIZATION_THRESHOLD) {
        // num_calls = CompileFunction(fn, arg);
        fn = num_calls;
    }
    
    curr_func.emplace(arg, num_calls);
    
    return fn;
}

static Module* MODULE = nullptr;

static Function* JIT_RESOLVE_FN;

// Adds JIT implementation functions to a module.
void declareInternalFunctions(llvm::LLVMContext& ctx, Module* module) {
    MODULE = module;

    JIT_RESOLVE_FN = Function::Create(
        FunctionType::get(Type::getInt64Ty(ctx), { Type::getInt64Ty(ctx), Type::getInt64Ty(ctx) }, false), 
        Function::ExternalLinkage, 
        "JITResolveCall", 
        module
    );
}

// Adds JIT implementation functions to dynamic linker.
void addInternalFunctions(MangleAndInterner& mangle, SymbolMap& map) {
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

// // Compiles a function specialized on a particular input.
// JITTargetAddress compileSpecialized(Function* function, JITTargetAddress arg) {
//     static ExecutionSession* ES;
//     assert(MODULE);
//     if (!FPM) {
//         FPM = std::make_unique<legacy::FunctionPassManager>(MODULE);
//         FPM->add(SPECIALIZE = new SpecializationPass());
//     }
//     ValueToValueMapTy vmap;
//     Function* copy = CloneFunction(function, vmap);
//     SPECIALIZE->setValue(arg);
//     FPM->run(*copy);
//     ES->dispatchMaterialization(MainJD, std::make_unique<MaterializationUnit>());
// }

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