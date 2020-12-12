#include "specializer.h"
#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace llvm::orc;
using namespace std;

static unordered_set<string> symbols;

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
    return fn; // TODO
}

// Inserts trampolines into functions. Transforms all function calls to active module functions
// into indirect calls, using the JITResolveCall function to resolve the address prior to invocation.
InstrumentationPass::InstrumentationPass(): FunctionPass(pid) {}

bool InstrumentationPass::runOnFunction(Function &f) {
    return true; // TODO
}