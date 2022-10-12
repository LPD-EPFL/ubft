#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

static void init(void) __attribute__((constructor));
static void init(void) {
  int ret = prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (ret == -1) {
    perror("Failed to set `PR_SET_PDEATHSIG`");
    exit(1);
  }
}
