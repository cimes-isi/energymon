#include <stdio.h>
#include <assert.h>
#include "em-default.h"

int main() {
  energymon impl;
  energymon_get_default(&impl);

  char source[100];
  impl.fsource(source);
  printf("Initializing reading from %s\n", source);
  assert(impl.finit(&impl) == 0);
  long long result = impl.fread(&impl);
  assert(result >= 0);
  printf("Got reading: %lld\n", result);
  assert(impl.ffinish(&impl) == 0);
  printf("Finished reading from %s\n", source);

  return 0;
}
