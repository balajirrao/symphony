#include "controller.hh"

#include <iostream>
#include <thread>
#include <fstream>
#include <regex>

void service(std::unique_ptr<Message> msg)
{
	char buf[20];

	sprintf(buf, "\\b%c\\w*\\b", msg->content[0]);
	std::regex pattern(buf);

	std::regex_iterator<std::string::const_iterator> it(msg->content.begin() + 2, msg->content.end(), std::move(pattern)), end;

	for (; it != end; it++)
		msg->reply(it->str() + "\n");
}

extern "C" void init()
{
	registerService("str.search", service);
}