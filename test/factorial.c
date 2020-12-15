int factorial(int n) {
    if (n == 0) return 1;
    return n * factorial(n - 1);
}

int main() {
    int sum = 0;
    for (int i = 0; i < 10000000; i ++) {
        sum += factorial(10);
    }
    return sum;
}