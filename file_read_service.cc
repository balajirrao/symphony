#include "controller.hh"

#include <iostream>
#include <thread>
#include <fstream>

void file_service(std::unique_ptr<Message> msg)
{
	log("file.read : got " + msg->content);
	//std::cout << "File.read " << msg->content << std::endl; 

	const char *fname = msg->content.c_str();
	std::ifstream file(fname);
	char buf[256 + 1];

	if (!file.good()) {
		log("file.read : bad file " + msg->content);
		return;
	}

	while (!file.eof()) {
		file.getline(buf, 256);
		//std::cout << "file.gcount = " << file.gcount() << std::endl;
		buf[file.gcount()] = 0;
		msg->reply(std::string(buf));
		//msg->reply("hello!");
		//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	log("file.read : done reading " + msg->content);

}

extern "C" void init()
{
	registerService("file.read", file_service);
}