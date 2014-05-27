CXX=g++ -std=c++11 -g -lpthread

services = hello_service.so file_read_service.so str_search_service.so file_search_service.so

all : libcontroller.so controller $(services)

libcontroller.so : controller.cc controller.hh logger.cc
	$(CXX) controller.cc logger.cc --shared -fPIC -o libcontroller.so

controller : libcontroller.so main.cc
	$(CXX) main.cc libcontroller.so -o controller -ldl

$(services) : %_service.so : libcontroller.so %_service.cc
	$(CXX) $^ --shared -fPIC -o $@

clean :
	 rm -f *.o *.so controller

.PHONY : all clean
