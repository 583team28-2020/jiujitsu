#include "stdio.h"

int unused() {
    return 2;
}

int fn(int n) {
    return n + 3; 
}

int main() {
    return fn(2);
}