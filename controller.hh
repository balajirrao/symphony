#ifndef CONTROLLER_HH
#define CONTROLLER_HH

#include <string>
#include <cstdbool>
#include <functional>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <mutex>

class Receiver {
public:
    std::queue <std::unique_ptr<std::string> > q;
    std::condition_variable cv;
    std::mutex mtx;
  	std::atomic<bool> closed;
  	int msgid;

    int fd;

    Receiver() : fd(0) , closed(false) {}

    std::unique_ptr<std::string> receive();
    ~Receiver();
};

struct Message
{
private:
  	static std::atomic<int> curr_id;

public:
  std::string service;
  bool is_reply;
  int id;
  std::string content;
  int reply_fd;
	std::shared_ptr<Receiver> rcvr;

  Message(const std::string service, const std::string content) :
            service(service), content(content),
            id(++curr_id), reply_fd(0), is_reply(false)
  {}

  ~Message();

  std::string serialise();

  std::unique_ptr<Message> recv_reply_for();
  void reply(const std::string &reply_content);
};

std::shared_ptr<Receiver> send_message(std::unique_ptr<Message> msg);

void registerService(const std::string& name, std::function<void(std::unique_ptr<Message> )> handler);
void registerExternalService(const std::string& name, char *where);

void log(std::string&&);

#endif
