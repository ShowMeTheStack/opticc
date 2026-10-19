// Unit tests for header prefetch: token bucket, held-set tracking, popularity ranking.
#include <string>
#include <vector>

#include "prefetch.hpp"
#include "test_harness.hpp"

using namespace opticc::prefetch;

OPTICC_TEST(prefetch_token_bucket) {
  TokenBucket b(1000.0, 500.0);               // 1000 B/s, burst 500 B
  CHECK_NEAR(b.available(10.0), 500.0, 1e-9); // starts full
  CHECK(b.take(400, 10.0));
  CHECK(!b.take(200, 10.0));                  // only 100 left
  CHECK_NEAR(b.available(10.1), 200.0, 1e-6); // +100 after 0.1 s
  CHECK_NEAR(b.available(20.0), 500.0, 1e-9); // capped at burst
  // never exceeds rate over a long window: 10 s at 1000 B/s + 500 burst
  TokenBucket c(1000.0, 500.0); double sent = 0;
  for (int i = 0; i <= 1000; ++i) { double t = i * 0.01; while (c.take(50, t)) sent += 50; }
  CHECK(sent <= 1000.0 * 10.0 + 500.0 + 1e-9);
  CHECK(sent >= 1000.0 * 10.0 - 100.0);
}

OPTICC_TEST(prefetch_held_set_tracking) {
  Tracker t(2);
  CHECK(!t.held(1, "a"));
  t.mark(1, "a", 100); t.mark(1, "a", 100);   // idempotent: bytes counted once
  CHECK(t.held(1, "a") && !t.held(2, "a"));
  CHECK(t.pushed_bytes(1) == 100);
  std::vector<std::string> shas{"a", "b", "c"};
  auto miss = t.missing(1, shas.begin(), shas.end());
  CHECK(miss.size() == 2 && miss[0] == "b" && miss[1] == "c");
  t.forget_node(1);
  CHECK(!t.held(1, "a") && t.held_count(1) == 0);
  CHECK(!t.held(7, "a"));                     // unknown node: never "held"
}

OPTICC_TEST(prefetch_popularity_ranking) {
  Tracker t(1);
  t.note("x", "/inc/x.h", 300);  t.note("x", "/inc/x.h", 300); t.note("x", "/inc/x.h", 300);   // 3 refs
  t.note("y", "/inc/y.h", 100);  t.note("y", "/inc/y.h", 100);                                  // 2 refs
  t.note("z", "/inc/z.h", 50);                                                                  // 1 ref
  auto p = t.pick(1, 1000, 10);
  CHECK(p.size() == 3 && p[0].first == "x" && p[1].first == "y" && p[2].first == "z");
  auto q = t.pick(1, 350, 10);               // budget: x (300) fits, y (100) would exceed, z (50) fits
  CHECK(q.size() == 2 && q[0].first == "x" && q[1].first == "z");
  t.mark(1, "x", 300);
  auto r = t.pick(1, 1000, 1);               // held files are never picked again; max_items honoured
  CHECK(r.size() == 1 && r[0].first == "y");
  auto top = t.top_paths(2);
  CHECK(top.size() == 2 && top[0] == "/inc/x.h" && top[1] == "/inc/y.h");
  t.note("w", "/inc/w.h", 10, 100);          // warm-start weight outranks live counts
  CHECK(t.top_paths(1)[0] == "/inc/w.h");
}
