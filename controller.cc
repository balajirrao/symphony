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
  if (reply_fd)
    close(reply_fd);

  if (rcvr && rcvr->is_local) {
        std::unique_lock<std::mutex> cvlck(rcvr->mtx);
        BOOST_LOG_TRIVIAL (trace) << "closing receiver";
        rcvr->closed.store(true);
        rcvr->cv.notify_all();
  }

  BOOST_LOG_TRIVIAL(trace) << "destroyed message " << Message::id;
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
  auto service = services[m->service];

  BOOST_LOG_TRIVIAL(trace) << "calling service handler for " << m->service;

  try {
    service(std::move(m));
  } catch (const std::exception &ex) {
    BOOST_LOG_TRIVIAL(error) << "exception in service handler - " << ex.what();
  }
}

void serviceIn(void)
{
    std::unique_lock<std::mutex> lck (inMutex);
    while (1) {
      BOOST_LOG_TRIVIAL(trace) << "inq : waiting for message";
      
      in_cv.wait(lck, [](){return !in.empty();});
      
      BOOST_LOG_TRIVIAL(trace) << "inq : message arrived";

      std::unique_ptr<Message> msg = std::move(in.front());
      in.pop();

      if (services.count (msg->service) == 0 )
        BOOST_LOG_TRIVIAL(error) << "no such service : " << msg->service;
      else
        std::thread (call_handler, std::move(msg)).detach();
    }
}

void queueIn(std::unique_ptr<Message> msg)
{
  std::unique_lock<std::mutex> lck (inMutex);
  in.push (std::move(msg));
  in_cv.notify_all();

  BOOST_LOG_TRIVIAL(trace) << "inq : queueing in message";
}

void serviceOut(void)
{
  std::unique_lock<std::mutex> lck (outMutex);

  while (1)
  {
    int ret;
    BOOST_LOG_TRIVIAL(trace) << "outq : waiting for message";
    out_cv.wait(lck, [](){return !out.empty();});
    BOOST_LOG_TRIVIAL(trace) << "outq : message arrived";

    std::unique_ptr<Message> msg = std::move(out.front());
    out.pop();

    if (send(msg->rcvr->fd, msg->serialise().c_str(), msg->serialise().length(), 0) < 0)
      BOOST_LOG_TRIVIAL(error) << "outq : sending message failed";
    else
      BOOST_LOG_TRIVIAL(trace) << "outq : sent message"; 
  }
}

void queueOut(std::unique_ptr<Message> msg)
{
  int ret;

   if (services.count (msg->service) == 0 ) {
      msg->rcvr->is_local = false;

      if (remote_services.count (msg->service) == 0)
        throw std::runtime_error("outq : no such service");

      int len = (remote_services[msg->service].second == AF_INET)
                  ? sizeof (struct sockaddr_in) : sizeof (struct sockaddr_un);

      int sd = socket(remote_services[msg->service].second, SOCK_STREAM, 0);
      if (sd < 0)
        throw std::runtime_error("outq : creating socket failed");

      if(connect (sd, remote_services[msg->service].first, len) < 0)
        throw std::runtime_error("outq : could not connect to remote service");

      msg->rcvr->fd = sd;
    } else {
      msg->rcvr->is_local = true;

      BOOST_LOG_TRIVIAL(debug) << "outq : in-queueing message";
      queueIn(std::move(msg));
      return;
    }

  std::unique_lock<std::mutex> lck (outMutex);
  out.push(std::move(msg));
  out_cv.notify_all();
}

static std::unique_ptr<std::string> read_len_prefixed_data(int fd)
{
  char buf[MSG_LEN_MAX + 1];

  std::stringstream sstream;
  char *colon_pos;
  int msg_len, total_len, n;

  n = recv (fd, buf, 6, MSG_PEEK);
  if (n < 0)
    throw std::runtime_error("recv error when peeking for length prefix");

  if (n == 0)
    return nullptr;

  colon_pos = strchr(buf, ':');
  if (colon_pos == NULL)
    throw std::runtime_error("invalid message format when peeking for length prefix - no length prefix");

  *colon_pos = 0;
  msg_len = atoi(buf);

  if (msg_len > MSG_LEN_MAX)
    throw std::runtime_error("message too long");

  total_len = msg_len + (colon_pos - buf) + 1;

  n = recv(fd, buf, total_len, MSG_WAITALL);
  if (n < 0)
    throw std::runtime_error("error when receiving messasge body");

  if (n < total_len)
    throw std::runtime_error("invalid message format when peeking for length prefix - message is short");

  buf[total_len] = 0;

  return std::unique_ptr<std::string>(new std::string (colon_pos + 1));
}

std::unique_ptr<std::string> Receiver::receive()
{
  if (!is_local) {    
    std::unique_ptr<std::string> msg;
    int n;

    BOOST_LOG_TRIVIAL(trace) << "trying to receive remote message";
    msg = read_len_prefixed_data(fd);

    return std::move(msg);
  } else {
    std::unique_ptr<std::string> s;

    BOOST_LOG_TRIVIAL(trace) << "trying to receive local message";

    std::unique_lock<std::mutex> lck(mtx);
    cv.wait(lck, [&]() {return (!q.empty() || closed.load());});

    if (q.empty())
      return nullptr;

    s = std::move(q.front());
    q.pop();

    return std::move(s);
  }
}

Receiver::~Receiver()
{
  BOOST_LOG_TRIVIAL(trace) << "destroying receiver for " << msgid; 
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
  if (reply_content.length() > MSG_LEN_MAX)
    throw std::runtime_error("reply : message too long");

  if (rcvr) {
    std::unique_lock<std::mutex> lck(rcvr->mtx);

    rcvr->q.push(std::unique_ptr<std::string>(new std::string(reply_content)));
    rcvr->cv.notify_all();   
  } else {
    char buf[8 + 1];

    /* TODO : Close if reply len is 0 ?*/
    snprintf(buf, 8, "%lu:", reply_content.length());

    if (send(reply_fd, buf, strlen(buf), 0) < 0)
      throw std::runtime_error("reply : could not send length prefix");

    /* TODO : should send whole message */
    if (send(reply_fd, reply_content.c_str(), reply_content.length(), 0) < 0)
      throw std::runtime_error("reply : could not send message");
  }
}

std::string Message::serialise()
{
  std::stringstream sstream;

  sstream << service.length() + content.length() + 1;
  sstream << ':' << service << ':' << content;

  return sstream.str();
}



void fd_reader(int fd)
{
  std::unique_ptr<std::string> msg;
  int len;
  size_t colon_pos;

  try {
    msg = read_len_prefixed_data(fd);    
  } catch (char *err) {
    BOOST_LOG_TRIVIAL(error) << err;
  }

  colon_pos = msg->find_first_of(':');

  if (colon_pos == std::string::npos)
    throw std::runtime_error("fd_reader : bad message format");

  std::unique_ptr<Message> m(new Message(msg->substr(0, colon_pos), msg->substr(colon_pos + 1, std::string::npos)));
  m->reply_fd = fd;
  queueIn(std::move(m));
  
}

void rpc_listener(int port)
{
  int sockfd, newfd;
  struct sockaddr_in adr_srvr;
  socklen_t len_inet;
  int c;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1)
    throw std::runtime_error("rpc_listener : could not create socket");

  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, NULL, 0);
  memset(&adr_srvr,0,sizeof adr_srvr);  

  adr_srvr.sin_family = AF_INET;  
  adr_srvr.sin_port = htons(port);  
  adr_srvr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sockfd, (struct sockaddr *)&adr_srvr, sizeof(adr_srvr)) < 0) {
    close (sockfd);
    throw std::runtime_error("rpc_listener : could not bind to socket");
  }

  if (listen(sockfd,10) < 0) {
    close (sockfd);
    throw std::runtime_error("rpc_listener : could not listen on socket");
  }

  for (;;) {
    BOOST_LOG_TRIVIAL(trace) << "rpc listener : waiting for connection";
    c = accept(sockfd, NULL, NULL);
     if ( c > 0 ) {
       std::thread(fd_reader, c).detach();
       BOOST_LOG_TRIVIAL(trace) << "rpc listener : accepted connection";
     } else
       BOOST_LOG_TRIVIAL(error) << "rpc_listener : accept failed";
    }  
}

void ipc_listener(const char *filename)
{
  int sockfd;
  struct sockaddr_un adr_srvr;
  socklen_t len_inet;
  int c;

  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd == -1)
    throw std::runtime_error("ipc_listener : could not create socket");

  memset(&adr_srvr,0,sizeof adr_srvr);  
  adr_srvr.sun_family = AF_UNIX;
  strncpy(adr_srvr.sun_path, filename, sizeof(adr_srvr.sun_path) - 1);

 if (bind(sockfd, (struct sockaddr *)&adr_srvr, sizeof(adr_srvr)) < 0) {
    close (sockfd);
    throw std::runtime_error("ipc_listener : could not bind to socket");
  }

  if (listen(sockfd,10) < 0) {
    close (sockfd);
    throw std::runtime_error("ipc_listener : could not listen on socket");
  }

  for (;;) {
  BOOST_LOG_TRIVIAL(trace) << "rpc listener : waiting for connection";
  c = accept(sockfd, NULL, NULL);
   if ( c > 0 ) {
     std::thread(fd_reader, c).detach();
     BOOST_LOG_TRIVIAL(trace) << "rpc listener : accepted connection";
   } else
     BOOST_LOG_TRIVIAL(error) << "rpc_listener : accept failed";
  }
}

void registerService(const std::string& name, std::function<void(std::unique_ptr<Message>)> handler)
{
  BOOST_LOG_TRIVIAL(debug) << "registered service " << name;
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
    pf = AF_UNIX;

  } else {
    char *colon_pos;
    struct sockaddr_in *inaddr = new struct sockaddr_in;
    int port;

    colon_pos = strchr(where, ':');
    if (colon_pos == nullptr) {
      free (addr);
      throw std::runtime_error("registerExternalService : bad address");
    }

    *colon_pos = 0;
    if (inet_aton(where, &inaddr->sin_addr) == 0) {
      free (addr);
      throw std::runtime_error("registerExternalService : bad address inet_aton");
    }

    port = atoi(colon_pos + 1);
    inaddr->sin_port = htons(port);
    inaddr->sin_family = AF_INET;

    addr = (struct sockaddr *) inaddr;
    pf = AF_INET;
  }

  BOOST_LOG_TRIVIAL(trace) << "registered external service " << name << " at " << where;

  remote_services[name] = std::make_pair(addr, pf);    

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

  std::thread(serviceIn).detach();
  std::thread(serviceOut).detach();

  std::thread(rpc_listener, port).detach();

  sprintf(fname, "/tmp/controllers/%d", port);
  unlink(fname);

  ipc_listener(fname);
}
