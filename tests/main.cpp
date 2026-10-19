#include "TestRunner.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
	if (argc == 1)
		return smulti_test::run_all();
	if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
		smulti_test::list_all();
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "--run") == 0)
		return smulti_test::run_one(argv[2]);
	std::printf("usage: streammulticast_tests [--list | --run <name>]\n");
	return 2;
}
