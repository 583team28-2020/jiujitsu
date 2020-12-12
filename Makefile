CC = clang++

LLVM_MODULES = 

<<<<<<< HEAD
CPPFLAGS = -O3 `llvm-config --cppflags $(LLVM_MODULES)`
LDFLAGS = `llvm-config --ldflags $(LLVM_MODULES)`
LIBS = `llvm-config --libs $(LLVM_MODULES)`
=======
CPPFLAGS = -g3 `llvm-config-10 --cppflags $(LLVM_MODULES)`
LDFLAGS = `llvm-config-10 --ldflags $(LLVM_MODULES)`
LIBS = `llvm-config-10 --libs $(LLVM_MODULES)`
>>>>>>> 70cf5be0b47b08129948a26bcc04ee3d43eaccbc

all: main
	$(CC) *.o $(LDFLAGS) $(LIBS) -o jiujitsu
main:
	$(CC) *.cpp -c $(CPPFLAGS)
# hash_test:
# 	$(CC) hash_test.cpp hash.cpp $(CPPFLAGS) $(LDFLAGS) $(LIBS) -o hash_test