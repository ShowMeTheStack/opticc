// SPDX-License-Identifier: GPL-2.0-only
// opticc: the coordinator's half of the shared direct manifests (fresh-client fast path; opticc/rdirect.hpp has
// the design and the soundness argument). Included by coordinator.cpp (member definitions of Coordinator, kept in
// their own file).
//
//   fetch    serve_client / serve_local_only, after the client-cache probe and BEFORE scheduling: the portable key P of
//            the request (wrapper's source hash + environment, compiler fingerprint, argv, cwd), asked of the worker
//            the key's hash selects among the workers whose HELLO Bloom filter may hold P (at most two asked);
//            entries are validated newest first against THIS machine's files (content, memoised; probed directories,
//            listed); the first valid one is served — object, dependency file, diagnostics — and recorded locally (the
//            next build's wrapper answers it). Any doubt (no key, refused fingerprint, timeout, no valid entry, object
//            integrity) falls through to the normal path.
//   publish  record_direct, after a LOCAL direct entry was written: queued, then sent by one background thread at the
//            lowest CPU priority, paced like the cache write-back (OPTICC_RDIRECT_MBPS, default 100, and 20% of the
//            node's upload bandwidth); drained for at most OPTICC_UPLOAD_DRAIN_S at exit. Entries recorded FROM a
//            fetched entry are never re-published.
//   OPTICC_RDIRECT=off disables both; OPTICC_RDIRECT_PUBLISH=off disables publishing only.

static bool rd_on() { static const bool on = util::getenv_or("OPTICC_RDIRECT", "on") != "off"; return on; }

std::string Coordinator::rd_pkey(const JobRec& r) {
  if (r.dsrc.empty() || r.cc_local.empty() || r.argv.empty()) return {};
  if (!rdirect::env_ok(r.denv)) { rd_refused_++; return {}; }
  std::error_code ec; const std::string cc = fs::canonical(r.cc_local, ec).string();
  if (ec || cc.empty()) { rd_refused_++; return {}; }
  // one toolchain identity: the compiler id of uploads and the system-include content fingerprint of every key
  const std::string cid = cachekey::compiler_id(cc);
  const std::string sysfp = sysinc_content(cc);
  std::vector<std::string> sysdirs;
  {
    std::lock_guard<std::mutex> g(rd_bloom_mu_);
    auto it = rd_sysdirs_.find(cc);
    if (it == rd_sysdirs_.end()) it = rd_sysdirs_.emplace(cc, sysinc::dirs_for(cc)).first;
    sysdirs = it->second;
  }
  std::string why;
  const std::string fp = rd_tc_.fingerprint(cc, r.argv, r.lang.empty() ? "c" : r.lang, dep_lists_system_headers(r.argv), cid, sysfp, sysdirs, &why);
  if (fp.empty()) { rd_refused_++; note_skip(r, "rdirect-refused", "fingerprint: " + why); return {}; }
  std::vector<std::string> argv = r.argv; argv[0] = r.cc_local;   // exactly the wrapper's direct-key argv
  return rdirect::pkey(fp, argv, r.cwd, r.denv, r.dsrc);
}

// the workers that can hold P: connected, enabled, with a published-manifest store; ordered by P's hash (the publisher
// uses the first); `filter` keeps only those whose HELLO Bloom filter may hold P
std::vector<int> Coordinator::rd_nodes(const std::string& pk, bool filter) {
  std::vector<int> c;
  for (size_t k = 1; k <= conns_.size(); ++k) {
    auto& w = conns_[k - 1];
    if (!w->connected() || sched_->node_disabled(static_cast<int>(k)) || !w->hello_ack().contains("rdirect")) continue;
    c.push_back(static_cast<int>(k));
  }
  if (c.empty()) return c;
  std::rotate(c.begin(), c.begin() + static_cast<long>(fnv64(pk) % c.size()), c.end());
  if (!filter) return c;
  std::vector<int> out;
  std::lock_guard<std::mutex> g(rd_bloom_mu_);
  for (int k : c) {
    if (rd_bloom_.size() <= static_cast<size_t>(k)) rd_bloom_.resize(static_cast<size_t>(k) + 1);
    auto& b = rd_bloom_[static_cast<size_t>(k)];
    if (!b.first) { b.first = true; b.second = bloom::Filter::from_json(conns_[static_cast<size_t>(k - 1)]->hello_ack().value("rdirect_bloom", json())); }
    if (b.second.n > 0 && b.second.maybe(pk)) out.push_back(k);
  }
  return out;
}

bool Coordinator::rd_get(int node, const std::string& pk, const std::string& okey, rdirect::ReplyMsg* rep, const std::string& kind) {
  auto& w = conns_[static_cast<size_t>(node - 1)];
  const int64_t id = (int64_t(1) << 52) + rd_next_id_++;
  std::shared_ptr<net::WorkerConn::HpSession> s;
  try { s = w->request_session(rdirect::encode_get(rdirect::GetMsg{id, pk, okey, kind}), id); } catch (...) { return false; }
  static const double to = std::max(0.05, std::atof(util::getenv_or("OPTICC_RDIRECT_TIMEOUT_S", "3").c_str()));
  std::unique_lock<std::mutex> l(s->m);
  if (!s->cv.wait_for(l, std::chrono::duration<double>(to), [&] { return s->done; })) { rd_timeouts_++; return false; }
  if (!s->has_raw) return false;
  try { *rep = rdirect::decode_reply(s->raw); } catch (...) { return false; }
  return rep->id == id;
}

bool Coordinator::try_remote_direct(const std::shared_ptr<JobRec>& r, const net::Socket& s) {
  if (!rd_on() || !local_cache_.enabled() || r->dkey.empty() || r->dsrc.empty() || conns_.empty()) return false;
  if (topo_.params.mode_policy == ModePolicy::Classic) return false;
  const auto t0 = std::chrono::steady_clock::now();
  struct Acc { std::atomic<uint64_t>& ns; std::chrono::steady_clock::time_point t; ~Acc() { ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count()); } } acc{rd_ns_, t0};
  const std::string pk = rd_pkey(*r);
  if (pk.empty()) return false;
  const std::vector<int> nodes = rd_nodes(pk, true);
  if (nodes.empty()) { rd_absent_++; return false; }
  rd_asked_++;
  std::string first_why;
  int asked = 0;
  for (int node : nodes) {
    if (++asked > 2) break;
    rdirect::ReplyMsg rep;
    auto tg = std::chrono::steady_clock::now();
    const bool got = rd_get(node, pk, "", &rep);
    rd_get_ns_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tg).count());
    if (!got || !rep.have) continue;
    std::vector<rdirect::PEntry> es;
    tg = std::chrono::steady_clock::now();
    const bool dec = rdirect::decode(rep.entries, &es);
    rd_dec_ns_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tg).count());
    if (!dec) continue;
    for (auto it = es.rbegin(); it != es.rend(); ++it) {
      if (it->pos.empty()) continue;   // a preprocessed-key entry (no inputs) is never served for a direct key
      std::string why; direct::Entry local;
      auto tv = std::chrono::steady_clock::now();
      const bool ok = rd_val_.valid(*it, &why, &local);
      rd_val_ns_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tv).count());
      if (!ok) { if (first_why.empty()) first_why = why; continue; }
      rdirect::Object obj = std::move(rep.obj);
      if (obj.okey != it->obj || obj.obj_sha != it->obj_sha) {   // the reply carried another entry's object: fetch this one's
        // by object key and sha: two publishers of one key with different bytes are both held
        rdirect::ReplyMsg r2;
        if (!rd_get(node, pk, it->obj + "@" + it->obj_sha, &r2)) continue;
        obj = std::move(r2.obj);
      }
      if (obj.okey != it->obj || obj.object.empty() || obj.rc != 0 || util::sha256_hex(obj.object) != it->obj_sha) { rd_integrity_++; continue; }
      // serve exactly as a client-cache hit (try_client_cache)
      util::write_file(closure::absolutize(r->out, r->cwd), obj.object);
      if (r->scan.has_md) {
        std::string tgt = r->scan.dep_target.empty() ? r->out : r->scan.dep_target;
        std::string dp = r->scan.dep_file.empty() ? args::default_dep_name(r->out) : r->scan.dep_file;
        util::write_file(closure::absolutize(dp, r->cwd), tgt + ":" + obj.depfile_body);
      }
      send(s, {{"op", "decision"}, {"node", -3}, {"name", "client-cache"}, {"mode", "header_push"}});
      send(s, {{"op", "result"}, {"rc", 0}, {"stdout", obj.out}, {"stderr", obj.err}, {"node", 0}, {"mode", "header_push"}});
      rd_hits_++; note_skip(*r, "rdirect-hit", "node " + std::to_string(node));
      // the next build's wrapper answers it by itself: object into the client cache, the validated entry recorded
      // locally (only when every input was settled — see Validator::valid); never published again
      {   // off the build's path: queued for the background thread that also publishes
        std::lock_guard<std::mutex> g(rd_mu_);
        rd_local_q_.push_back(RdLocal{r->dkey, std::move(local), std::move(obj)}); rd_cv_.notify_one();
      }
      std::lock_guard<std::mutex> g(mu_);
      Job j; j.id = static_cast<int>(wl_.jobs.size()); j.name = r->src; j.src_size_bytes = 0; j.pre_size_bytes = 0;
      wl_.jobs.push_back(j); r->id = j.id; sched_->on_jobs_added();   // accounted, never offered to the scheduler
      r->t_request = r->t_decided = r->t_done = now(); r->rc = 0; r->outcome = "client-cache"; r->cache_status = "rdirect-hit"; r->obj_bytes = obj.object.size();
      jobs_.push_back(r); last_activity_ = now();
      return true;
    }
  }
  rd_misses_++;
  if (!first_why.empty()) note_skip(*r, "rdirect-miss", first_why);
  return false;
}

// Only direct keys are shared. Preprocessed-text keys are not: the text may contain .incbin directives, and files
// probed only by __has_include appear in no dependency file. TUs with a computed __has_include are cached only
// locally (preprocessed-text key).

void Coordinator::rd_publish_async(const std::shared_ptr<JobRec>& r, const direct::Entry& e) {
  static const bool pub = util::getenv_or("OPTICC_RDIRECT_PUBLISH", "on") != "off";
  if (!rd_on() || !pub || conns_.empty() || r->dsrc.empty()) return;
  std::lock_guard<std::mutex> g(rd_mu_);
  if (rd_q_.size() >= 100000) { rd_pub_dropped_++; return; }
  rd_q_.push_back(RdTask{r, e}); rd_pub_queued_++; rd_cv_.notify_one();
}

void Coordinator::rd_publish_one(RdTask& t) {
  const std::string pk = rd_pkey(*t.r);
  if (pk.empty()) return;
  const std::vector<int> nodes = rd_nodes(pk, false);
  if (nodes.empty()) { rd_pub_dropped_++; return; }
  const int node = nodes[0];
  objcache::Hit hit;
  if (!local_cache_.lookup_object(t.entry.obj, &hit) || hit.rc != 0 || hit.object.empty()) { rd_pub_dropped_++; return; }
  rdirect::PutMsg m; m.pkey = pk; m.entry = rdirect::from_direct(t.entry, "");
  // exact-bytes hashes (PPos::fsha): only for files whose stat is still the recorded (settled) one, so the bytes hashed
  // now are the bytes the entry was recorded with
  for (size_t i = 0; i < t.entry.pos.size(); ++i) {
    const auto& p = t.entry.pos[i];
    const direct::FStat now = direct::stat_path(p.path);
    if (direct::same(now, p.st)) m.entry.pos[i].fsha = rd_val_.hash_of(p.path, now, rdirect::Validator::Fast);
  }
  m.obj.okey = m.entry.obj; m.obj.obj_sha = util::sha256_hex(hit.object); m.entry.obj_sha = m.obj.obj_sha;
  m.obj.object = std::move(hit.object); m.obj.depfile_body = std::move(hit.depfile_body); m.obj.out = std::move(hit.out); m.obj.err = std::move(hit.err); m.obj.rc = 0;
  uint64_t sent = 0;
  try { conns_[static_cast<size_t>(node - 1)]->send_frame(rdirect::encode_put(m, &sent)); }
  catch (...) { rd_pub_failed_++; return; }
  rd_pub_sent_++; rd_pub_bytes_ += sent;
  if (!rd_draining_.load()) {   // pace: the build's own traffic first (as the cache write-back)
    static const double cap_bps = std::max(1.0, std::atof(util::getenv_or("OPTICC_RDIRECT_MBPS", "100").c_str())) * 1e6;
    double bw; { std::lock_guard<std::mutex> g(mu_);
      bw = (static_cast<size_t>(node) < live_up_bps_.size() && live_up_bps_[static_cast<size_t>(node)] > 0) ? live_up_bps_[static_cast<size_t>(node)] : topo_.node(node).bw_up_bps; }
    const double rate = std::min(cap_bps, bw > 0 ? 0.2 * bw : cap_bps);
    std::this_thread::sleep_for(std::chrono::duration<double>(8.0 * static_cast<double>(sent) / rate));
  }
}

void Coordinator::rd_store_local(RdLocal& t) {
  auto ts = std::chrono::steady_clock::now();
  try {
    const bool stored = local_cache_.has_object(t.obj.okey) || local_cache_.store(t.obj.okey, t.obj.object, t.obj.depfile_body, t.obj.out, t.obj.err, 0);
    // no direct entry is recorded unless its object is held
    if (stored && !t.entry.obj.empty()) { direct::record(local_cache_.dir(), t.dkey, t.entry); rd_recorded_++; }
  } catch (...) {}
  rd_store_ns_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ts).count());
}

void Coordinator::rd_publish_worker() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
  ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
#endif
  for (;;) {
    RdTask t; RdLocal lt; bool local = false;
    {
      std::unique_lock<std::mutex> l(rd_mu_);
      rd_cv_.wait(l, [&] { return !rd_q_.empty() || !rd_local_q_.empty(); });
      if (!rd_local_q_.empty()) { lt = std::move(rd_local_q_.front()); rd_local_q_.pop_front(); local = true; }   // local stores first
      else { t = std::move(rd_q_.front()); rd_q_.pop_front(); }
      rd_busy_++;
    }
    if (local) rd_store_local(lt);
    else { try { rd_publish_one(t); } catch (...) { rd_pub_failed_++; } }
    { std::lock_guard<std::mutex> l(rd_mu_); rd_busy_--; }
    rd_cv_.notify_all();
  }
}

void Coordinator::rd_drain() {
  const double limit = std::atof(util::getenv_or("OPTICC_UPLOAD_DRAIN_S", "20").c_str());
  rd_draining_ = true;
  std::unique_lock<std::mutex> l(rd_mu_);
  rd_cv_.wait_for(l, std::chrono::duration<double>(std::max(0.0, limit)), [&] { return rd_q_.empty() && rd_local_q_.empty() && rd_busy_ == 0; });
  rd_pub_dropped_ += rd_q_.size();
  while (!rd_local_q_.empty()) { RdLocal t = std::move(rd_local_q_.front()); rd_local_q_.pop_front(); l.unlock(); rd_store_local(t); l.lock(); }
}

void Coordinator::rd_drain_local() {
  std::unique_lock<std::mutex> l(rd_mu_);
  rd_cv_.wait_for(l, std::chrono::seconds(30), [&] { return rd_local_q_.empty() && rd_busy_ == 0; });
}

// the Validator's CPU / hash-kind profile (thread CPU vs wall per phase; digests per kind)
static json rd_validator_json(const rdirect::Validator& v) {
  auto s = [](const std::atomic<uint64_t>& ns) { return static_cast<double>(ns.load()) / 1e9; };
  json kinds = json::object();
  const char* names[3] = {"raw", "key", "fast"};
  for (int k = 0; k < 3; ++k)
    kinds[names[k]] = {{"n", v.n_digest[k].load()}, {"bytes", v.bytes_digest[k].load()}, {"cpu_s", s(v.cpu_digest[k])}, {"wall_s", s(v.ns_digest[k])}};
  return json{{"cpu_s", {{"stat", s(v.cpu_stat)}, {"hash", s(v.cpu_hash)}, {"neg", s(v.cpu_neg)}}},
              {"wall_s", {{"stat", s(v.ns_stat)}, {"hash", s(v.ns_hash)}, {"neg", s(v.ns_neg)}}},
              {"digest", kinds}, {"memo_hits", v.memo_hits.load()}, {"distinct_files", v.distinct_files()},
              {"no_fsha", v.no_fsha.load()}, {"fsha_mismatch", v.fsha_mismatch.load()}, {"positives", v.positives.load()},
              {"entries", v.entries.load()}, {"listed_dirs", v.listed_dirs()},
              {"neg_access", v.neg_access.load()}, {"neg_listed", v.neg_listed.load()},
              {"neg_access_why", {{"dir_missing", v.neg_access_why[0].load()}, {"not_pinned", v.neg_access_why[1].load()},
                                  {"dot_component", v.neg_access_why[2].load()}, {"last_listed", v.neg_access_why[3].load()}}},
              {"shared_digest_waits", v.shared_waits.load()}};
}

json Coordinator::rd_summary() {
  size_t left; { std::lock_guard<std::mutex> g(rd_mu_); left = rd_q_.size(); }
  return json{{"hits", rd_hits_.load()}, {"misses", rd_misses_.load()}, {"asked", rd_asked_.load()}, {"absent", rd_absent_.load()},
              {"refused", rd_refused_.load()}, {"timeouts", rd_timeouts_.load()}, {"integrity", rd_integrity_.load()},
              {"recorded_locally", rd_recorded_.load()}, {"fetch_s", static_cast<double>(rd_ns_.load()) / 1e9},
              {"phase_s", {{"get", static_cast<double>(rd_get_ns_.load()) / 1e9}, {"decode", static_cast<double>(rd_dec_ns_.load()) / 1e9},
                           {"validate", static_cast<double>(rd_val_ns_.load()) / 1e9}, {"store_local", static_cast<double>(rd_store_ns_.load()) / 1e9},
                           {"v_stat", static_cast<double>(rd_val_.ns_stat.load()) / 1e9}, {"v_hash", static_cast<double>(rd_val_.ns_hash.load()) / 1e9},
                           {"v_neg", static_cast<double>(rd_val_.ns_neg.load()) / 1e9}}},
              {"hashed_files", rd_val_.hashed_files()}, {"listed_dirs", rd_val_.listed_dirs()},
              {"validator", rd_validator_json(rd_val_)},
              {"published", {{"queued", rd_pub_queued_.load()}, {"sent", rd_pub_sent_.load()}, {"bytes", rd_pub_bytes_.load()},
                             {"dropped", rd_pub_dropped_.load()}, {"failed", rd_pub_failed_.load()}, {"left", left}}}};
}
