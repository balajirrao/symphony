#include "controller.hh"

#include <iostream>
#include <thread>
#include <fstream>

void file_service(std::unique_ptr<Message> msg)
{
	BOOST_LOG_TRIVIAL(trace) << "file.read : got message " << msg->id;

	const char *fname = msg->content.c_str();
	std::ifstream file(fname);
	char buf[256 + 2];

	if (!file.good())
		throw std::runtime_error("file.read : bad file " + msg->content);

	while (!file.eof()) {
		file.getline(buf, 256);
		buf[file.gcount() - 1] = '\n';
		buf[file.gcount()] = 0;
		msg->reply(std::string(buf));
	}

	BOOST_LOG_TRIVIAL(trace) << "file.read : done reading for message " << msg->id;

}

extern "C" void init()
{
	registerService("file.read", file_service);
}