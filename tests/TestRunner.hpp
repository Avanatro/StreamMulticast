// Minimal header-only test runner. No external dependencies, no OBS startup.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include <type_traits>

namespace smulti_test {

struct TestCase {
	std::string name;
	std::function<void()> fn;
};

inline std::vector<TestCase> &registry()
{
	static std::vector<TestCase> tests;
	return tests;
}

inline int &failure_count()
{
	static int failures = 0;
	return failures;
}

struct Registrar {
	Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void report_failure(const char *file, int line, const std::string &msg)
{
	std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
	++failure_count();
}

inline std::string to_text(const std::string &v) { return "\"" + v + "\""; }
inline std::string to_text(const char *v) { return std::string("\"") + v + "\""; }
inline std::string to_text(bool v) { return v ? "true" : "false"; }
template<typename T, typename = std::enable_if_t<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value>>
std::string to_text(T v) { return std::to_string(v); }
template<typename T, typename = std::enable_if_t<std::is_enum<T>::value>>
std::string to_text(T v, int = 0) { return std::to_string(static_cast<long long>(v)); }

inline int run_all()
{
	int failed_tests = 0;
	for (auto &t : registry()) {
		int before = failure_count();
		std::printf("[ RUN  ] %s\n", t.name.c_str());
		t.fn();
		if (failure_count() != before) {
			std::printf("[ FAIL ] %s\n", t.name.c_str());
			++failed_tests;
		} else {
			std::printf("[  OK  ] %s\n", t.name.c_str());
		}
	}
	std::printf("%zu tests, %d failed\n", registry().size(), failed_tests);
	return failed_tests == 0 ? 0 : 1;
}

// Prints one registered case name per line.
inline void list_all()
{
	for (auto &t : registry())
		std::printf("%s\n", t.name.c_str());
}

// Runs a single case by name. Returns 0 pass, 1 fail, 2 unknown name.
inline int run_one(const std::string &name)
{
	for (auto &t : registry()) {
		if (t.name != name)
			continue;
		int before = failure_count();
		std::printf("[ RUN  ] %s\n", t.name.c_str());
		t.fn();
		if (failure_count() != before) {
			std::printf("[ FAIL ] %s\n", t.name.c_str());
			return 1;
		}
		std::printf("[  OK  ] %s\n", t.name.c_str());
		return 0;
	}
	std::printf("unknown case: %s\n", name.c_str());
	return 2;
}

} // namespace smulti_test

#define SMULTI_TEST(name)                                                                  \
	static void name();                                                                \
	static smulti_test::Registrar registrar_##name(#name, name);                       \
	static void name()

#define EXPECT_TRUE(cond)                                                                  \
	do {                                                                               \
		if (!(cond))                                                               \
			smulti_test::report_failure(__FILE__, __LINE__, "expected true: " #cond); \
	} while (0)

#define EXPECT_EQ(actual, expected)                                                        \
	do {                                                                               \
		auto a_ = (actual);                                                        \
		auto e_ = (expected);                                                      \
		if (!(a_ == e_))                                                           \
			smulti_test::report_failure(__FILE__, __LINE__,                    \
				std::string(#actual " != " #expected " (actual: ") +       \
				smulti_test::to_text(a_) + ", expected: " + smulti_test::to_text(e_) + ")"); \
	} while (0)
