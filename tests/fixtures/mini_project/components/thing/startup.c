#include <stdio.h>

// Nothing references this file, so only a whole-archive link keeps it.
__attribute__((constructor)) static void announce(void) { printf("constructor ran\n"); }
