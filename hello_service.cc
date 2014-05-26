#include "controller.hh"

#include <iostream>
#include <thread>

void hello_service(std::unique_ptr<Message> msg)
{
	std::cout << "hello sevice : got " << msg->content << std::endl;
	msg->reply("Hello " + std::move(msg->content) + '\n');
}

extern "C" void init()
{
	registerService("hello", hello_service);
}