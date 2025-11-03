#include "kernel/types.h"
#include "user/user.h"

int main(void) {
  int ncpu = 4;

  for (int i = 0; i < ncpu; i++) {
    if (fork() == 0) {
      int r = rwlktest();
      exit(r);
    }
  }

  int passed = 0;
  for (int i = 0; i < ncpu; i++) {
    int status;
    wait(&status);
    if (status == 0)
      passed++;
  }

  printf("rwlktest: %d/%d CPUs succeeded\n", passed, ncpu);
  exit(0);
}
