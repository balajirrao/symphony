#include "controller.hh"
#include <dlfcn.h>
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <cstring>

extern void init_controller(int port);
extern void stop_controller(int port);
extern void start_logger();
extern void stop_logger();

static int port;

void sigint_handler(int signo)
{
  if (signo == SIGINT)
    printf("received SIGINT\n");
    stop_controller(port);
    log ("stopping logger");
    stop_logger();
    _Exit(0);
}

int main(int argc, char *argv[])
{
  int i;
  port = atoi(argv[1]);

  start_logger();

  for (i = 2; i < argc; i++)
  {
    if (strstr(argv[i], ".so") == NULL)
      break;

    void *handle = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
    void (*init)() = (void (*)()) dlsym(handle, "init");

    init();
  }

  for (;i < argc; i++)
  {
    char *at_pos = strchr(argv[i], '@');
    *at_pos = 0;

    registerExternalService(argv[i], at_pos + 1);
  }

  if (signal(SIGINT, sigint_handler) == SIG_ERR)
    log("can't catch SIGINT");

  signal(SIGPIPE, SIG_IGN);


  init_controller(port);

  return 0;
}
