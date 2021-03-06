#include "controller.hh"

#include <iostream>
#include <thread>
#include <fstream>
#include <regex>
#include <memory>

void service(std::unique_ptr<Message> msg)
{
	std::string filename = msg->content.substr(2, std::string::npos);
	char c = msg->content[0];

	std::unique_ptr<Message> mfile (new Message("file.read", filename));
	std::shared_ptr<Receiver> rcvrfile = send_message(std::move(mfile));
	std::unique_ptr<std::string> strfile;

	while ((strfile =  rcvrfile->receive()) != nullptr) {
		std::unique_ptr<Message> msrch (new Message("str.search", (std::string(1, c) + ":") + *std::move(strfile)));
		auto rcvrsrch = send_message(std::move(msrch));

		std::unique_ptr<std::string> strsrch;

		while ((strsrch = rcvrsrch->receive()) != nullptr) {
			msg->reply(*strsrch);	
		}
	}
}

extern "C" void init()
{
	registerService("file.search", service);
}