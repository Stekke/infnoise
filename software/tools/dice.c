// This is a simple program to read from a binary file of random bits and generate
// dice rolls.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Role a single die.
static void rollDie(uint32_t sides, FILE *file) {
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
  printf(" %u", randVal + 1u);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    printf("Usage: dice randFile numDice numSides\n");
    return 1;
  }
  FILE *file = fopen(argv[1], "r");
  if (file == NULL) {
    fprintf(stderr, "Unable to open file %s\n", argv[1]);
    return 1;
  }
  int numDice = atoi(argv[2]);
  int numSides = atoi(argv[3]);
  if (numDice < 1 || numSides < 1) {
    fprintf(stderr, "numDice and numSides must be numbers above 0\n");
    return 1;
  }
  uint32_t dice = numDice;
  uint32_t sides = numSides;
  uint32_t i;
  printf("Rolling %u %u-sided dice:", dice, sides);
  for (i = 0u; i < dice; i++) {
    rollDie(sides, file);
  }
  printf("\n");
  fclose(file);
  return 0;
}
