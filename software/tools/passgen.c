// This is a simple program to read from a binary file of random bits and generate
// an alphabetic password.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// Role a single die.
static uint32_t rollDie(uint32_t sides, FILE *file) {
  uint32_t randVal;
  do {
    uint32_t numBytes = 0u;
    randVal = 0u;
    while (numBytes < 4u && 1u << (8u*numBytes) <= sides) {
      numBytes++;
      int c = getc(file);
      if (c == EOF) {
        printf("Ran out of random data\n");
        exit(1);
      }
      randVal = (randVal << 8u) | (uint32_t)c;
    }
  } while(randVal >= sides);
  return randVal;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    printf("Usage: passgen randFile numKeys\n");
    return 1;
  }
  FILE *file = fopen(argv[1], "r");
  if (file == NULL) {
    fprintf(stderr, "Unable to open file %s\n", argv[1]);
    return 1;
  }
  int numKeys = atoi(argv[2]);
  if (numKeys < 1) {
    fprintf(stderr, "numKeys must be a number above 0\n");
    return 1;
  }
  uint32_t keys = numKeys;
  uint32_t i;
  printf("password:");
  for (i = 0u; i < keys; i++) {
    uint32_t randVal = rollDie(26u, file);
    putchar('a' + randVal);
  }
  printf("\nThis password has %.2f bits of entropy\n", keys*log(26.0)/log(2.0));
  fclose(file);
  return 0;
}
