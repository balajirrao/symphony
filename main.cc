#include "controller.hh"
#include <dlfcn.h>
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <cstring>

extern void init_controller(int port);
extern void stop_controller(int port);

static int port;

void sigint_handler(int signo)
{
  if (signo == SIGINT)
    BOOST_LOG_TRIVIAL(trace) << "received SIGINT. exiting";
    stop_controller(port);
    _Exit(0);
}

void sigpipe_handler (int s)
{
  BOOST_LOG_TRIVIAL(error) << "SIGPIPE";
}

int main(int argc, char *argv[])
{
  int i;
  port = atoi(argv[1]);

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
    BOOST_LOG_TRIVIAL(error) << "can't catch SIGINT";

  signal(SIGPIPE, sigpipe_handler);


  init_controller(port);

  return 0;
}
