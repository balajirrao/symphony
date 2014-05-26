#include "controller.hh"
#include <dlfcn.h>
#include <iostream>
#include <unistd.h>

extern void init_controller(int port);
extern void stop_controller(int port);

static int port;

void sigint_handler(int signo)
{
  if (signo == SIGINT)
    printf("received SIGINT\n");
    stop_controller(port);

    exit(0);
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
  printf("\ncan't catch SIGINT\n");

  init_controller(port);

  /*
  Message m("hello", "", "world");

  m.id = 10;
  send(m);

  Message m2 = recvReplyFor(m);
  std::cout << "Got : " << m2.content << " " << std::endl;

  Message m3 = recvReplyFor(m);
  std::cout << "Got again : " << m3.content << " " << std::endl;

  //std::this_thread::sleep_for (std::chrono::seconds(1)); */

  return 0;
}
