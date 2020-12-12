CC = clang++

LLVM_MODULES = 

CPPFLAGS = -O3 `llvm-config --cppflags $(LLVM_MODULES)`
LDFLAGS = `llvm-config --ldflags $(LLVM_MODULES)`
LIBS = `llvm-config --libs $(LLVM_MODULES)`

all: main
	$(CC) *.o $(LDFLAGS) $(LIBS) -o jiujitsu
main:
	$(CC) *.cpp -c $(CPPFLAGS)
# hash_test:
# 	$(CC) hash_test.cpp hash.cpp $(CPPFLAGS) $(LDFLAGS) $(LIBS) -o hash_test