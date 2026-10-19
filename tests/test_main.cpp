// Minimal test harness: each test registers itself; the binary runs one test
// by name (used by ctest) or all tests when invoked without arguments.
// SPDX-License-Identifier: GPL-2.0-only
#include "test_harness.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

namespace opticc_test {
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
int failures = 0;
}

int main(int argc, char** argv) {
  using namespace opticc_test;
  int ran = 0, failed = 0;
  for (auto& t : registry()) {
    if (argc > 1 && std::strcmp(argv[1], t.name) != 0) continue;
    failures = 0;
    try { t.fn(); } catch (const std::exception& e) { std::printf("  exception: %s\n", e.what()); failures++; }
    std::printf("[%s] %s\n", failures ? "FAIL" : " OK ", t.name);
    ran++; if (failures) failed++;
  }
  if (ran == 0) { std::printf("no such test\n"); return 2; }
  return failed ? 1 : 0;
}
