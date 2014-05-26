#include "controller.hh"
#include <queue>
#include <mutex>
#include <string>
#include <unordered_map>
#include <iostream>
#include <thread>
#include <functional>
#include <condition_variable>
#include <chrono>
#include <sys/types.h>  
#include <sys/socket.h>  
#include <sys/un.h>
#include <netinet/in.h>  
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include <dlfcn.h>
#include <atomic>
#include <memory>
#include <sstream>
#include <dirent.h>
#include <fstream>

std::atomic<int> Message::curr_id(0);

Message::~Message()
{
    std::cout << "Message destroyed" << std::endl;

  if (reply_fd) {
    close(reply_fd);
  } else {
        rcvr->closed.store(true);
            std::unique_lock<std::mutex> cvlck(rcvr->mtx);

        rcvr->cv.notify_all();
  }
}

static std::queue<std::unique_ptr<Message> > in;
static std::mutex inMutex;

static std::queue<std::unique_ptr<Message> > out;
static std::mutex outMutex;

static std::unordered_map <std::string, std::function<void(std::unique_ptr<Message>)> > services;

static std::unordered_map <std::string, std::pair<struct sockaddr *, socklen_t>> remote_services;

void call_handler(std::unique_ptr<Message> m)
{
  services[m->service](std::move(m));
}

void serviceIn(void)
{
    std::unique_lock<std::mutex> lck (inMutex);

    //std::cout << "ServiceIn running" << std::endl;

    while (!in.empty()) {
      std::unique_ptr<Message> msg = std::move(in.front());
      in.pop();

      if (services.count (msg->service) == 0 )
        std::cout << "ERR : No such service : " << msg->service << std::endl;
      else
          std::thread (call_handler, std::move(msg)).detach();
    }
}

void queueIn(std::unique_ptr<Message> msg)
{
  std::unique_lock<std::mutex> lck (inMutex);
  in.push (std::move(msg));
  std::thread(serviceIn).detach();
}

void serviceOut(void)
{
  std::unique_lock<std::mutex> lck (outMutex);

  //std::cout << "ServiceOut running" << std::endl;

  while (!out.empty())
  {
    int ret;

    std::unique_ptr<Message> msg = std::move(out.front());
    out.pop();
    ret = send(msg->rcvr->fd, msg->serialise().c_str(), msg->serialise().length(), 0);

    if (ret == -1) {
      std::cout << "ServiceOut : sending message failed\n";
      perror(NULL);
    }
  }
}

void queueOut(std::unique_ptr<Message> msg)
{
  int ret;

   if (services.count (msg->service) == 0 ) { /* Nonlocal request ? */

      if (remote_services.count (msg->service) == 0) 
        std::cout << "Message : no such external service " << msg->service << std::endl; 
      else {
        int len = (remote_services[msg->service].second == AF_INET)
                    ? sizeof (struct sockaddr_in) : sizeof (struct sockaddr_un);

        int sd = socket(remote_services[msg->service].second, SOCK_STREAM, 0);

        ret = connect (sd, remote_services[msg->service].first, len);
        if (ret == -1) {
          std::cout << "connect failed when sending message" << std::endl;
          perror(NULL);
        }
        msg->rcvr->fd = sd;
      }
    } else {
        queueIn(std::move(msg));
        return;
    }

  std::unique_lock<std::mutex> lck (outMutex);
  out.push(std::move(msg));
  std::thread(serviceOut).detach();
}

std::unique_ptr<std::string> Receiver::receive()
{
  if (fd) {
    char buf[256 + 1];

    std::cout << "going to remote receive : " << std::endl;
    int n = read(fd, buf, 256);

    std::cout << "going to remote receive : done " << n << std::endl;

    buf[n] = 0;

    if (n == 0)
      return nullptr;
    else
      return std::unique_ptr<std::string> (new std::string(buf));
  } else {
    std::unique_lock<std::mutex> lck(q_mtx);

    cv.wait(lck, [&]() {return (!q.empty() || closed.load());});
/*
    if (q.empty() && !closed.load()) {
      //lck.unlock();
      //std::unique_lock<std::mutex> cvlck (mtx);
      cv.wait(lck, !q.empty());
    } */

    std::unique_ptr<std::string> s(nullptr);

    if (!q.empty()) {
      s = std::move(q.front());
      q.pop();
    }

    return std::move(s);
  }
}

Receiver::~Receiver()
{
  std::cout << "Receiver destroyed for msgid " << msgid << " " << std::endl;
  if (fd)
    close (fd);
}

std::shared_ptr<Receiver> send_message(std::unique_ptr<Message> msg)
{
  std::shared_ptr<Receiver> rcvr = std::make_shared<Receiver>();

  msg->rcvr = rcvr;
  msg->rcvr->msgid = msg->id;

  //if (msg->from_fd)
  //  rcvr->fd = dup(msg->from_fd);

  queueOut(std::move(msg));

  return rcvr;
}

void Message::reply(const std::string &reply_content)
{
  //std::cout << "Replying.. to " << rcvr->msgid << std::endl;
  if (rcvr) {/* Local reply ? */
    std::unique_lock<std::mutex> lck(rcvr->q_mtx);
    //std::unique_lock<std::mutex> cvlck(rcvr->mtx);

    rcvr->q.push(std::unique_ptr<std::string>(new std::string(reply_content)));
    rcvr->cv.notify_all();   
  } else {
    printf("Remote remply\n");
    int ret = write(reply_fd, reply_content.c_str(), reply_content.length());

    printf("write reeturned %d\n", ret);
  }
}

std::string Message::serialise()
{
  return service + ":" + content; 
}

void fd_reader(int fd)
{
  char buf[256 + 1];
  size_t len;
  char* colon_pos;
  char* next_colon_pos;

  len = read(fd, buf, 256);
  if (buf[len - 1] == '\n')
    buf[len - 1] = 0;
  else
    buf[len] = 0;

  //printf("Received %s\n", buf);

  colon_pos = strchr(buf, ':');
  *colon_pos = 0;

    std::unique_ptr<Message> m(new Message(std::string(buf), std::string(colon_pos + 1)));
    m->reply_fd = fd;
    queueIn(std::move(m));
  
}

void rpc_listener(int port)
{
  int sockfd, newfd;
  struct sockaddr_in adr_srvr;
  struct sockaddr_in* adr_clnt = new sockaddr_in();
  socklen_t len_inet;
  int c;

  sockfd = socket(PF_INET, SOCK_STREAM, 0);
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, NULL, 0);
  memset(&adr_srvr,0,sizeof adr_srvr);  

  adr_srvr.sin_family = AF_INET;  
  adr_srvr.sin_port = htons(port);  
  adr_srvr.sin_addr.s_addr = htonl(INADDR_ANY);

  bind(sockfd, (struct sockaddr *)&adr_srvr, sizeof(adr_srvr));
  listen(sockfd,10);  

  for (;;) {  
      /* 
       * Wait for a connect: 
       */  
           //len_inet = sizeof adr_clnt;  
           c = accept(sockfd,  
                    (struct sockaddr *) adr_clnt,  
                    &len_inet);  
       
           if ( c == -1 ) {  
               printf("accept(2)");  
           }  

           //fd_reader(c, adr_clnt, len_inet);
           std::thread(fd_reader, c).detach();
    }  
}

void ipc_listener(const char *filename)
{
  int sockfd;
  struct sockaddr_un adr_srvr;
  struct sockaddr_un* adr_clnt = new sockaddr_un();
  socklen_t len_inet;
  int c;

  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  memset(&adr_srvr,0,sizeof adr_srvr);  
  strncpy(adr_srvr.sun_path, filename, sizeof(adr_srvr.sun_path) - 1);

  bind(sockfd, (struct sockaddr *)&adr_srvr, sizeof(adr_srvr));
  listen(sockfd,10);

  for (;;) {  
      /* 
       * Wait for a connect: 
       */  
           c = accept(sockfd,  
                    NULL, NULL); 
           std::cout << "Accepted with " << ((struct sockaddr_un *) adr_clnt) -> sun_path  << std::endl;
       
           if ( c == -1 ) {  
               printf("accept(2)");  
           }  

           std::thread(fd_reader, c).detach();
    }   
}

void registerService(const std::string& name, std::function<void(std::unique_ptr<Message>)> handler)
{
  services[name] = handler;
}

void registerExternalService(const std::string& name, char *where)
{
  struct sockaddr *addr;
  int af;
  int ret;

  char *slash_pos;

  slash_pos = strchr(where, '/');
  if (slash_pos) {
    struct sockaddr_un *a = new (struct sockaddr_un);

    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, where, sizeof (a->sun_path));

    addr = (struct sockaddr *) a;
    af = AF_UNIX;

  } else {
    char *colon_pos;
    struct sockaddr_in *inaddr = new struct sockaddr_in;
    int port;

    colon_pos = strchr(where, ':');
    if (colon_pos == nullptr) {
      std::cout << "Bad address" << std::endl;
    }

    *colon_pos = 0;
    inet_aton(where, &inaddr->sin_addr);

    port = atoi(colon_pos + 1);
    inaddr->sin_port = htons(port);

    inaddr->sin_family = AF_INET;

    addr = (struct sockaddr *) inaddr;
    af = AF_INET;
  }

  remote_services[name] = std::make_pair(addr, af);    

}

void stop_controller(int port)
{
  char fname[32];

  sprintf(fname, "/tmp/controllers/%d", port);
  unlink(fname); 
}

void init_controller(int port)
{
  char fname[32];

  std::thread(rpc_listener, port).detach();

  sprintf(fname, "/tmp/controllers/%d", port);
  unlink(fname);

  ipc_listener(fname);
}
