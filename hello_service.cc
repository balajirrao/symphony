#include "controller.hh"

#include <iostream>
#include <thread>

void hello_service(std::unique_ptr<Message> msg)
{
	msg->reply("Hello " + std::move(msg->content) + '\n');
}

extern "C" void init()
{
	registerService("hello", hello_service);
}