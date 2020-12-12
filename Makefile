CC = clang++

LLVM_MODULES = 

CPPFLAGS = -g3 `llvm-config-10 --cppflags $(LLVM_MODULES)`
LDFLAGS = `llvm-config-10 --ldflags $(LLVM_MODULES)`
LIBS = `llvm-config-10 --libs $(LLVM_MODULES)`

all: main
	$(CC) *.o $(LDFLAGS) $(LIBS) -o jiujitsu
main:
	$(CC) *.cpp -c $(CPPFLAGS)