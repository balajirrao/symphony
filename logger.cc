#include "controller.hh"
#include <queue>
#include <string>
#include <condition_variable>
#include <iostream>
#include <thread>

static std::queue<std::string> q;
static std::mutex mtx;
static std::condition_variable cv;
static bool stop= false;

#include <time.h> // time_t, tm, time, localtime, strftime

// Returns the local date/time formatted as 2014-03-19 11:11:52
char* getFormattedTime(void) {

    time_t rawtime;
    struct tm* timeinfo;

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    // Must be static, otherwise won't work
    static char _retval[20];
    strftime(_retval, sizeof(_retval), "%Y-%m-%d %H:%M:%S ", timeinfo);

    return _retval;
}

void worker()
{
	std::unique_lock<std::mutex> lck(mtx);

	while (1) {
		cv.wait(lck, [](){return !q.empty() || stop;});
		if (stop)
			return;
		std::cerr << q.front() << std::endl;
		q.pop();
	}	
}

void log(std::string &&str)
{
	std::unique_lock<std::mutex> lck(mtx);
	q.push(getFormattedTime() + std::move(str));
	cv.notify_all();
}

void start_logger()
{
	std::thread(worker).detach();
}

void stop_logger()
{
	std::unique_lock<std::mutex> lck(mtx);
	stop = true;
	cv.notify_all();
}