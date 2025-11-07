#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define NCHILD 2
#define NCHILD4 4
#define N1 100000
#define N2 1000
#define NPAGES2 128
#define N3 10000
#define N4 100000
#define SZ 4096

void test1(void);
void test2(void);
void test3(void);
void test4(void);
char buf[SZ];

int countfree();

int main(int argc, char *argv[]) {
  test1();
  test2();
  test3();
  test4();
  exit(0);
}

int ntas(int print) {
  int n;
  char *c;

  if (statistics(buf, SZ) <= 0) {
    fprintf(2, "ntas: no stats\n");
  }
  c = strchr(buf, '=');
  n = atoi(c + 2);
  if (print)
    printf("%s", buf);
  return n;
}

// Test concurrent kallocs and kfrees
void test1(void) {
  void *a, *a1;
  int n, m;

  printf("start test1\n");
  m = ntas(0);
  for (int i = 0; i < NCHILD; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("fork failed");
      exit(-1);
    }
    if (pid == 0) {
      for (i = 0; i < N1; i++) {
        a = sbrk(4096);
        *(int *)(a + 4) = 1;
        a1 = sbrk(-4096);
        if (a1 != a + 4096) {
          printf("test1: FAIL wrong sbrk\n");
          exit(1);
        }
      }
      exit(0);
    }
  }
  int status = 0;
  for (int i = 0; i < NCHILD; i++) {
    wait(&status);
    if (status != 0) {
      printf("FAIL: a child failed\n");
      exit(1);
    }
  }
  printf("test1 results:\n");
  n = ntas(1);
  if (n - m < 10)
    printf("test1 OK\n");
  else
    printf("test1 FAIL\n");
}

// Test stealing
void test2() {
  int free0 = countfree();
  int free1;
  int n = (PHYSTOP - KERNBASE) / PGSIZE;
  printf("start test2\n");
  printf("total free number of pages: %d (out of %d)\n", free0, n);

  // allocate most of the memory upfront, so that the test iterations
  // are in the regime where the system is mostly out of memory.
  if (free0 < NPAGES2) {
    printf("test2 FAILED: not enough free memory");
    exit(1);
  }
  uint64 sz0 = (uint64)sbrk((free0 - NPAGES2) * PGSIZE);
  if (sz0 == (uint64)SBRK_ERROR) {
    printf("test2 FAILED: cannot allocate memory");
    exit(1);
  }
  free0 = NPAGES2;

  for (int i = 0; i < N2; i++) {
    free1 = countfree();
    if ((i + 1) % 100 == 0)
      printf(".");
    if (free1 != free0) {
      printf("test2 FAIL: losing pages %d %d\n", free0, free1);
      exit(1);
    }
  }
  sbrk(-((uint64)sbrk(0) - sz0));
  printf("\ntest2 OK\n");
}

// Test concurrent kalloc/kfree and stealing, for correctness
void test3(void) {
  printf("start test3\n");
  int pid;
  int free0 = countfree();

  for (int i = 0; i < NCHILD; i++) {
    pid = fork();
    if (pid < 0) {
      printf("fork failed");
      exit(-1);
    }
    if (pid == 0) {
      if (i == 0) {
        for (i = 0; i < N3; i++) {
          int cpid = fork();
          if (cpid < 0) {
            // Could be because we're out of memory.
            continue;
          }

          if (cpid == 0) {
            sbrk(4096);
            exit(0);
          }

          int status = 0;
          wait(&status);
          if ((i + 1) % 1000 == 0) {
            printf(".");
          }
        }
        printf("child done %d\n", i);
        exit(0);
      } else {
        while (1) {
          // Allocate all available memory, then release it.
          countfree();
        }
      }
    }
  }

  int status = 0;
  for (int i = 0; i < NCHILD - 1; i++) {
    wait(&status);
    if (status != 0) {
      printf("a child failed\n");
      exit(1);
    }
  }
  kill(pid);
  wait(&status);

  int free1 = countfree();
  if (free0 != free1) {
    printf("test3 FAIL: losing pages %d %d\n", free0, free1);
    exit(1);
  }

  printf("\ntest3 OK\n");
}

// Test concurrent kalloc/kfree and stealing, for performance
void test4(void) {
  uint64 a, a1;
  int n, m;

  m = ntas(0);
  printf("start test4\n");
  int pid;

  int npages = countfree();
  if (npages < 100) {
    printf("too few pages: %d\n", npages);
    exit(-1);
  }
  // Leave some headroom to avoid running out of memory or
  // contending on the last few available pages.
  npages -= (NCHILD4 - 1) * 100;

  for (int i = 0; i < NCHILD4; i++) {
    pid = fork();
    if (pid < 0) {
      printf("fork failed");
      exit(-1);
    }
    if (pid == 0) {
      cpupin(i);

      if (i < NCHILD4 - 1) {
        for (i = 0; i < N4; i++) {
          a = (uint64)sbrk(4096);
          if (a == 0xffffffffffffffff) {
            // no freemem
            continue;
          }
          *(int *)(a + 4) = 1;
          a1 = (uint64)sbrk(-4096);
          if (a1 != a + 4096) {
            printf("test3 FAIL: wrong sbrk\n");
            exit(1);
          }
          if ((i + 1) % 10000 == 0) {
            printf(".");
          }
        }
        printf("child done %d\n", i);
        exit(0);
      } else {
        while (1) {
          a = (uint64)sbrk(npages * 4096);
          if (a == 0xffffffffffffffff) {
            printf("test4 FAIL: cannot allocate %d pages\n", npages);
            exit(1);
          }

          sbrk(-npages * 4096);
        }
      }
    }
  }

  int status = 0;
  for (int i = 0; i < NCHILD4 - 1; i++) {
    wait(&status);
    if (status != 0) {
      printf("a child failed\n");
      exit(1);
    }
  }
  kill(pid);
  wait(&status);

  n = ntas(1);
  if (n - m < (NCHILD4 - 1) * 10000)
    printf("\ntest4 OK\n");
  else
    printf("test4 FAIL m %d n %d\n", m, n);
}

// use sbrk() to count how many free physical memory pages there are.
int countfree() {
  int n = 0;
  uint64 sz0 = (uint64)sbrk(0);
  while (1) {
    char *a = sbrk(PGSIZE);
    if (a == SBRK_ERROR) {
      break;
    }
    n += 1;
  }
  sbrk(-((uint64)sbrk(0) - sz0));
  return n;
}
