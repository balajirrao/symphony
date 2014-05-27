CXX=g++ -std=c++11 -lpthread -lboost_log-mt -DBOOST_ALL_DYN_LINK=1

services = hello_service.so file_read_service.so str_search_service.so file_search_service.so

all : libcontroller.so controller $(services)

libcontroller.so : controller.cc controller.hh
	$(CXX) controller.cc --shared -fPIC -o libcontroller.so

controller : libcontroller.so main.cc
	$(CXX) main.cc libcontroller.so -o controller -ldl

$(services) : %_service.so : libcontroller.so %_service.cc
	$(CXX) $^ --shared -fPIC -o $@

clean :
	 rm -f *.o *.so controller

.PHONY : all clean
