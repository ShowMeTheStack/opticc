// opticc-gentrace: synthetic workload generator.
// s ~ lognormal(median 1.5 MB, sigma 0.8); w = a + b*s_MB * lognormal noise;
// p = 0.15 w; q = q_frac * p (header-push client cost); o = 0.08 s;
// m = 300 MB + 50 * s; one generated-header job that every compile depends on,
// and one final link barrier.
// Header universe (header-push store simulation): H headers with
// lognormal sizes; each TU includes k headers drawn by Zipf popularity plus m
// rare ones drawn uniformly from the tail; closure = sum of their sizes;
// s_src = lognormal source bytes; s (preprocessed) = s_src + closure (+10% expansion).
// Without --headers the legacy sizing (s_src = 0.12 s, closure = 0.8 s) is kept.
// SPDX-License-Identifier: GPL-2.0-only
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "opticc/model.hpp"

using namespace opticc;

int main(int argc, char** argv) {
  int n = 2500; uint64_t seed = 1; std::string out = "trace.json";
  double median_mb = 1.5, sigma_s = 0.8, a = 0.3, b = 0.9, noise_sigma = 0.3;
  double gen_w = 2.0, link_w = 5.0, link_per_job = 0.002, q_frac = 0.05;
  int H = 0, k_common = 15, m_rare = 2; double zipf_s = 1.0, hdr_median_kb = 8, hdr_sigma = 1.0, src_median_kb = 20;
  for (int i = 1; i < argc; ++i) {
    std::string x = argv[i];
    auto next = [&]() { return std::string(argv[++i]); };
    if (x == "--n") n = std::stoi(next()); else if (x == "--seed") seed = std::stoull(next());
    else if (x == "--out") out = next(); else if (x == "--median-mb") median_mb = std::stod(next());
    else if (x == "--sigma") sigma_s = std::stod(next()); else if (x == "--a") a = std::stod(next());
    else if (x == "--b") b = std::stod(next()); else if (x == "--noise") noise_sigma = std::stod(next());
    else if (x == "--link-w") link_w = std::stod(next());
    else if (x == "--q-frac") q_frac = std::stod(next());
    else if (x == "--headers") H = std::stoi(next()); else if (x == "--k-common") k_common = std::stoi(next());
    else if (x == "--m-rare") m_rare = std::stoi(next()); else if (x == "--zipf") zipf_s = std::stod(next());
    else if (x == "--hdr-median-kb") hdr_median_kb = std::stod(next()); else if (x == "--hdr-sigma") hdr_sigma = std::stod(next());
    else if (x == "--src-median-kb") src_median_kb = std::stod(next());
    else { std::fprintf(stderr, "usage: opticc-gentrace [--n N] [--seed S] [--out f] [--median-mb M] [--sigma S] [--a A] [--b B] [--noise S] [--link-w W]\n"
                                "                       [--q-frac F] [--headers H --k-common K --m-rare M --zipf S --hdr-median-kb KB --hdr-sigma S --src-median-kb KB]\n"); return 2; }
  }
  std::mt19937_64 eng(seed);
  std::lognormal_distribution<double> ls(std::log(median_mb * 1e6), sigma_s);
  std::lognormal_distribution<double> lw(0.0, noise_sigma);
  Workload wl;
  // header universe: popularity ~ 1/rank^zipf_s over ids 0..H-1
  std::vector<double> cum;
  if (H > 0) {
    std::lognormal_distribution<double> lh(std::log(hdr_median_kb * 1e3), hdr_sigma);
    for (int h = 0; h < H; ++h) wl.header_sizes.push_back(lh(eng));
    double acc = 0; for (int h = 0; h < H; ++h) { acc += 1.0 / std::pow(h + 1.0, zipf_s); cum.push_back(acc); }
  }
  std::lognormal_distribution<double> lsrc(std::log(src_median_kb * 1e3), 0.8);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  Job gen; gen.id = 0; gen.name = "gen-headers"; gen.work_ref_cpu_s = gen_w; gen.mem_bytes = 200e6;
  wl.jobs.push_back(gen);
  for (int i = 1; i <= n; ++i) {
    Job j; j.id = i; j.name = "tu" + std::to_string(i);
    if (H > 0) {
      std::vector<int> hs;
      auto add = [&](int h) { for (int x : hs) if (x == h) return; hs.push_back(h); };
      for (int c = 0; c < k_common; ++c) {
        double r = u01(eng) * cum.back();
        add(static_cast<int>(std::lower_bound(cum.begin(), cum.end(), r) - cum.begin()));
      }
      int tail0 = H / 2;
      for (int c = 0; c < m_rare && tail0 < H; ++c) add(tail0 + static_cast<int>(u01(eng) * (H - tail0)) % (H - tail0));
      j.headers = hs;
      for (int h : hs) j.closure_bytes += wl.header_sizes[static_cast<size_t>(h)];
      j.src_size_bytes = lsrc(eng);
      j.pre_size_bytes = 1.1 * (j.src_size_bytes + j.closure_bytes);
    } else {
      j.pre_size_bytes = ls(eng);
      j.src_size_bytes = 0.12 * j.pre_size_bytes;
      j.closure_bytes = 0.8 * j.pre_size_bytes;
    }
    j.work_ref_cpu_s = (a + b * j.pre_size_bytes / 1e6) * lw(eng);
    j.preprocess_cpu_s = 0.15 * j.work_ref_cpu_s;
    j.scan_cpu_s = q_frac * j.preprocess_cpu_s;
    j.obj_size_bytes = 0.08 * j.pre_size_bytes;
    j.mem_bytes = 300e6 + 50 * j.pre_size_bytes;
    j.deps = {0};
    wl.jobs.push_back(j);
  }
  Job link; link.id = n + 1; link.name = "link"; link.is_link_barrier = true;
  link.work_ref_cpu_s = link_w + link_per_job * n; link.mem_bytes = 2e9;
  for (int i = 1; i <= n; ++i) link.deps.push_back(i);
  wl.jobs.push_back(link);
  save_json(out, json(wl));
  std::printf("wrote %s: %d compile jobs, total work %.1f ref-s, mean s %.2f MB, mean w %.2f s\n", out.c_str(), n,
              wl.total_work(), wl.mean_pre_size() / 1e6, wl.mean_work());
  return 0;
}
