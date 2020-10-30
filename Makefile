CC = clang++

LLVM_MODULES = 

CPPFLAGS = -g3 `llvm-config --cppflags $(LLVM_MODULES)`
LDFLAGS = `llvm-config --ldflags $(LLVM_MODULES)`
LIBS = `llvm-config --libs $(LLVM_MODULES)`

all:
	$(CC) *.o $(LDFLAGS) $(LIBS) -o jiujitsu
main:
	$(CC) *.cpp -c $(CPPFLAGS)