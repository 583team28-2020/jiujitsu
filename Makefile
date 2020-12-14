CC = clang++

LLVM_MODULES = 

CPPFLAGS = -g3 -fPIC `llvm-config-11 --cppflags $(LLVM_MODULES)`
LDFLAGS = `llvm-config-11 --ldflags $(LLVM_MODULES)`
LIBS = `llvm-config-11 --libs $(LLVM_MODULES)`

all: main
	$(CC) *.o $(LDFLAGS) $(LIBS) -o jiujitsu
main:
	$(CC) *.cpp -c $(CPPFLAGS)
# hash_test:
# 	$(CC) hash_test.cpp hash.cpp $(CPPFLAGS) $(LDFLAGS) $(LIBS) -o hash_test