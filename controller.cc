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
  log ("destroy message");

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
static std::condition_variable in_cv;

static std::queue<std::unique_ptr<Message> > out;
static std::mutex outMutex;
static std::condition_variable out_cv;

static std::unordered_map <std::string, std::function<void(std::unique_ptr<Message>)> > services;

static std::unordered_map <std::string, std::pair<struct sockaddr *, socklen_t>> remote_services;

void call_handler(std::unique_ptr<Message> m)
{
  services[m->service](std::move(m));
}

void serviceIn(void)
{
    std::unique_lock<std::mutex> lck (inMutex);
    while (1) {
      log ("serviceIn : waiting for incoming message");
      in_cv.wait(lck, [](){return !in.empty();});
      log ("serviceIn : incoming message");

      std::unique_ptr<Message> msg = std::move(in.front());
      in.pop();

      if (services.count (msg->service) == 0 )
        log ("serviceIn : no such service " + msg->service);
      else
        std::thread (call_handler, std::move(msg)).detach();
    }
}

void queueIn(std::unique_ptr<Message> msg)
{
  std::unique_lock<std::mutex> lck (inMutex);
  in.push (std::move(msg));
  in_cv.notify_all();
}

void serviceOut(void)
{
  std::unique_lock<std::mutex> lck (outMutex);

  while (1)
  {
    int ret;
    log ("serviceOut : waiting for incoming message");
    out_cv.wait(lck, [](){return !out.empty();});
    log ("serviceOut : incoming message");

    std::unique_ptr<Message> msg = std::move(out.front());
    out.pop();
    ret = send(msg->rcvr->fd, msg->serialise().c_str(), msg->serialise().length(), 0);

    if (ret == -1)
      log ("ServiceOut : sending message failed " + std::string(strerror(errno)));
  }
}

void queueOut(std::unique_ptr<Message> msg)
{
  int ret;

   if (services.count (msg->service) == 0 ) { /* Nonlocal request ? */

      if (remote_services.count (msg->service) == 0) 
        log("queueOut : no such external service " + msg->service);
      else {
        int len = (remote_services[msg->service].second == PF_INET)
                    ? sizeof (struct sockaddr_in) : sizeof (struct sockaddr_un);

        int sd = socket(remote_services[msg->service].second, SOCK_STREAM, 0);

        ret = connect (sd, remote_services[msg->service].first, len);
        if (ret == -1) {
          log("queueOut : connect failed" + std::string(strerror(errno)));
          msg->rcvr->closed = true;
        }

        msg->rcvr->fd = sd;
      }
    } else {
        queueIn(std::move(msg));
        return;
    }

  std::unique_lock<std::mutex> lck (outMutex);
  out.push(std::move(msg));
  out_cv.notify_all();
}

std::unique_ptr<std::string> Receiver::receive()
{
  if (closed)
    return nullptr;

  if (fd) {
    char buf[256 + 1];

    int n = read(fd, buf, 256);
    if (n <= 0)
      return nullptr;
    else {
      buf[n] = 0;
      return std::unique_ptr<std::string> (new std::string(buf));
    }

  } else {
    std::unique_lock<std::mutex> lck(q_mtx);

    cv.wait(lck, [&]() {return (!q.empty() || closed.load());});

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
  if (fd)
    close (fd);
}

std::shared_ptr<Receiver> send_message(std::unique_ptr<Message> msg)
{
  std::shared_ptr<Receiver> rcvr = std::make_shared<Receiver>();

  msg->rcvr = rcvr;
  msg->rcvr->msgid = msg->id;

  queueOut(std::move(msg));

  return rcvr;
}

void Message::reply(const std::string &reply_content)
{
  if (rcvr) {
    std::unique_lock<std::mutex> lck(rcvr->q_mtx);

    rcvr->q.push(std::unique_ptr<std::string>(new std::string(reply_content)));
    rcvr->cv.notify_all();   
  } else {
    int ret = write(reply_fd, reply_content.c_str(), reply_content.length());
    if (ret < 0)
      log ("write failed during reply");
  }
}

std::string Message::serialise()
{
  return service + ":" + content; 
}

void fd_reader(int fd)
{
  char buf[256 + 1];
  int len;
  char* colon_pos;
  char* next_colon_pos;

  len = read(fd, buf, 256);
  if (len < 0) {
    log ("fd_reader : read " + std::string (strerror(errno)));
    return;
  } else if (len < 1) {
    log ("fd_reader : couldn't read message. too short. quitting");
    return;
  }

  if (buf[len - 1] == '\n')
    buf[len - 1] = 0;
  else
    buf[len] = 0;

  colon_pos = strchr(buf, ':');
  *colon_pos = 0;

  if (colon_pos == NULL) {
    log ("fd_reader : bad message format");
    return;
  }

  std::unique_ptr<Message> m(new Message(std::string(buf), std::string(colon_pos + 1)));
  m->reply_fd = fd;
  queueIn(std::move(m));
  
}

void rpc_listener(int port)
{
  int sockfd, newfd;
  struct sockaddr_in adr_srvr;
  socklen_t len_inet;
  int c;

  sockfd = socket(PF_INET, SOCK_STREAM, 0);
  if (sockfd == -1) {
    log ("rpc_listener : " + std::string(strerror(errno)));
    return;
  }

  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, NULL, 0);
  memset(&adr_srvr,0,sizeof adr_srvr);  

  adr_srvr.sin_family = AF_INET;  
  adr_srvr.sin_port = htons(port);  
  adr_srvr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sockfd, (struct sockaddr *)&adr_srvr, sizeof(adr_srvr)) < 0) {
    log("rpc_listener bind : " + std::string(strerror(errno)));
    return;
  }

  if (listen(sockfd,10) < 0) {
    log("rpc_listener listen : " + std::string(strerror(errno)));
    return;
  }

  for (;;) {
    log ("rpc_listener : waiting for connection");
    c = accept(sockfd, NULL, NULL);

     if ( c > 0 ) {
       std::thread(fd_reader, c).detach();
       log ("rpc_listener : accepted");
     } else
       log ("rpc_listener : accept failed " + std::string(strerror(errno)));
       std::thread(fd_reader, c).detach();
    }  
}

void ipc_listener(const char *filename)
{
  int sockfd;
  struct sockaddr_un adr_srvr;
  socklen_t len_inet;
  int c;

  log ("start ipc listener");
  sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
  memset(&adr_srvr,0,sizeof adr_srvr);  
  adr_srvr.sun_family = AF_UNIX;
  strncpy(adr_srvr.sun_path, filename, sizeof(adr_srvr.sun_path) - 1);

  if (bind(sockfd, (struct sockaddr *)&adr_srvr, sizeof(adr_srvr)) < 0) {
    log("ipc_listener bind : " + std::string(strerror(errno)));
    return;
  }

  if (listen(sockfd,10) < 0) {
    log("ipc_listener listen : " + std::string(strerror(errno)));
    return;
  }

  for (;;) {  
     log ("ipc_listener : waiting for connection");
     c = accept(sockfd, NULL, NULL);

     if ( c > 0 ) {
       std::thread(fd_reader, c).detach();
       log ("ipc_listener : accepted");
     } else
       log ("ipc_listener : accept failed " + std::string(strerror(errno)));
  }  
}

void registerService(const std::string& name, std::function<void(std::unique_ptr<Message>)> handler)
{
  log ("registered service " + name);
  services[name] = handler;
}

void registerExternalService(const std::string& name, char *where)
{
  struct sockaddr *addr;
  int pf;
  int ret;

  char *slash_pos;

  slash_pos = strchr(where, '/');
  if (slash_pos) {
    struct sockaddr_un *a = new (struct sockaddr_un);

    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, where, sizeof (a->sun_path));

    addr = (struct sockaddr *) a;
    pf = PF_UNIX;

  } else {
    char *colon_pos;
    struct sockaddr_in *inaddr = new struct sockaddr_in;
    int port;

    colon_pos = strchr(where, ':');
    if (colon_pos == nullptr) {
      log ("registerExternalService : bad address");
      return;
    }

    *colon_pos = 0;
    if (inet_aton(where, &inaddr->sin_addr) == 0) {
      log ("registerExternalService : bad address inet_aton");
      return;
    }

    port = atoi(colon_pos + 1);
    inaddr->sin_port = htons(port);

    inaddr->sin_family = AF_INET;

    addr = (struct sockaddr *) inaddr;
    pf = PF_INET;
  }

  log ("registered external service " + name + " at " + where);

  remote_services[name] = std::make_pair(addr, pf);    

}

void stop_controller(int port)
{
  char fname[32];

  //log ("stop controller");

  sprintf(fname, "/tmp/controllers/%d", port);
  unlink(fname); 
}

void init_controller(int port)
{
  char fname[32];

  std::thread(serviceIn).detach();
  std::thread(serviceOut).detach();


  log ("start controller");
  std::thread(rpc_listener, port).detach();

  sprintf(fname, "/tmp/controllers/%d", port);
  unlink(fname);

  ipc_listener(fname);
}
