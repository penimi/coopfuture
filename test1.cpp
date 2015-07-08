#include <iostream>
#include <exception>
#include <functional>
#include <boost/asio.hpp>

#include "coopfuture.hpp"

int blah(int a) {
	std::cout << "inner\n";
}

int main() {
	boost::asio::io_service io;
	auto ff = std::bind(
	  (std::size_t(boost::asio::io_service::*)())
	    &boost::asio::io_service::run_one,
	  &io);
	fut::Scheduler sched(io);
	auto f = sched.spawn(std::bind(&blah, 23));
	try {
		f->await();
	} catch (std::exception e) {
		std::cout << "error\n";
	}
}
