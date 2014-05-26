#include "controller.hh"

#include <iostream>
#include <thread>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>

void file_service(std::unique_ptr<Message> msg)
{
	//std::cout << "File.read " << msg->content << std::endl; 

	const char *fname = msg->content.c_str();
	char buf[256 + 1];

	int fd = open(fname, O_RDONLY);
	int nread;

	while ((nread = read(fd, buf, 256)) > 0) {
		std::cout << "read : " << nread << std::endl;
		buf[nread] = 0;
		msg->reply(std::string(buf));
		//msg->reply("hello!");
		//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	std::cout << "File.read exit for " << msg->id << std::endl; 

}

extern "C" void init()
{
	registerService("file.read", file_service);
}