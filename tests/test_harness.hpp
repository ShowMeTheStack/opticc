// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cmath>
#include <cstdio>
#include <vector>

namespace opticc_test {
struct TestCase { const char* name; void (*fn)(); };
std::vector<TestCase>& registry();
extern int failures;
struct Registrar { Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); } };
}  // namespace opticc_test

#define OPTICC_TEST(name) \
  static void name(); \
  static opticc_test::Registrar name##_reg(#name, name); \
  static void name()

#define CHECK(cond) do { if (!(cond)) { std::printf("  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); opticc_test::failures++; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (a), _b = (b); if (std::fabs(_a - _b) > (tol)) { std::printf("  CHECK_NEAR failed: %s=%.9g vs %s=%.9g tol=%g (%s:%d)\n", #a, _a, #b, _b, (double)(tol), __FILE__, __LINE__); opticc_test::failures++; } } while (0)
#define CHECK_REL(a, b, rel) do { double _a = (a), _b = (b); if (std::fabs(_a - _b) > (rel) * std::fabs(_b)) { std::printf("  CHECK_REL failed: %s=%.9g vs %s=%.9g rel=%g (%s:%d)\n", #a, _a, #b, _b, (double)(rel), __FILE__, __LINE__); opticc_test::failures++; } } while (0)
