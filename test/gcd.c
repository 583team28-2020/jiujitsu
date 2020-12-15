#include "stdlib.h"

int gcd(int a, int b) {
    if (b == 0) return a;
    return gcd(b, a % b);
}

int main() {
    int sum = 0;
    for (int i = 0; i < 10000000; i ++) {
        sum += gcd(492816303l, 21123692l);
    }
    return sum;
}