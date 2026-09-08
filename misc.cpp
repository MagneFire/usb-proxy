#include <math.h>
#include <time.h>

#include "misc.h"

double uptime_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string hexToAscii(std::string input) {
	std::string output = input;
	size_t pos = output.find("\\x");
	while (pos != std::string::npos) {
		std::string substr = output.substr(pos + 2, 2);

		std::istringstream iss(substr);
		iss.flags(std::ios::hex);
		int i;
		iss >> i;
		output = output.replace(pos, 4, 1, char(i));
		pos = output.find("\\x");
	}
	return output;
}

int hexToDecimal(int input) {
	int output = 0;
	int i = 0;
	while (input != 0) {
		output += (input % 10) * pow(16, i);
		input /= 10;
		i++;
	}
	return output;
}
