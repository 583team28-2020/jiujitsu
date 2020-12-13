#include "stdio.h"

int unused() {
    return 2;
}

int fn(int n) {
    return n + 3; 
}

int main() {
    int i = fn(2);
    int j = fn(3);
    int k = fn(4);
    return i + j + k;
}