#include <stdio.h>

int main() {
    int value = getchar();
    int count = 0;
    while(value != EOF) {
        printf("%02x", value);
        value = getchar();
        if(++count == 64) {
            putchar('\n');
            count = 0;
        }
    }
    if(count != 0) {
        putchar('\n');
    }
    return 0;
}
