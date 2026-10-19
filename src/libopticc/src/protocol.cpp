// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/protocol.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace opticc::proto {

void put_u32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>((v >> 24) & 0xff)); out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff)); out.push_back(static_cast<char>(v & 0xff));
}
uint32_t get_u32(const char* p) {
  auto b = reinterpret_cast<const unsigned char*>(p);
  return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}

std::string pack(const json& header, const std::string& blob) {
  std::string h = header.dump(), out;
  out.reserve(4 + h.size() + blob.size());
  put_u32(out, static_cast<uint32_t>(h.size()));
  out += h; out += blob;
  return out;
}
void unpack(const std::string& payload, json* header, std::string* blob) {
  if (payload.size() < 4) throw std::runtime_error("short blob frame");
  uint32_t h = get_u32(payload.data());
  if (4 + static_cast<size_t>(h) > payload.size()) throw std::runtime_error("bad header length");
  *header = json::parse(payload.substr(4, h));
  if (blob) *blob = payload.substr(4 + h);
}
int64_t frame_id(const Frame& f) {
  try {
    json h;
    try { unpack(f.payload, &h, nullptr); } catch (const std::exception&) { h = json::parse(f.payload); }
    if (h.is_object() && h.contains("id") && h["id"].is_number_integer()) return h["id"].get<int64_t>();
  } catch (const std::exception&) {}
  return -1;
}

std::string zstd_compress(const std::string& raw, int level) {
  size_t bound = ZSTD_compressBound(raw.size());
  std::string out(bound, '\0');
  size_t n = ZSTD_compress(out.data(), bound, raw.data(), raw.size(), level);
  if (ZSTD_isError(n)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(n));
  out.resize(n);
  return out;
}
size_t max_blob_bytes() {
  static const size_t v = [] {
    const char* e = std::getenv("OPTICC_MAX_BLOB_MB");
    long long mb = e ? std::atoll(e) : 0;
    return static_cast<size_t>(mb > 0 ? mb : 512) << 20;
  }();
  return v;
}
std::string zstd_decompress(const std::string& z, size_t raw_len) {
  if (raw_len > max_blob_bytes()) throw std::runtime_error("zstd: declared length exceeds OPTICC_MAX_BLOB_MB");
  if (raw_len == 0 && z.empty()) return std::string();
  const unsigned long long fcs = ZSTD_getFrameContentSize(z.data(), z.size());
  if (fcs == ZSTD_CONTENTSIZE_ERROR) throw std::runtime_error("zstd: not a zstd frame");
  if (fcs != ZSTD_CONTENTSIZE_UNKNOWN && fcs != raw_len) throw std::runtime_error("zstd: declared length does not match the frame");
  std::string out(raw_len, '\0');
  size_t n = ZSTD_decompress(out.data(), raw_len, z.data(), z.size());
  if (ZSTD_isError(n)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(n));
  if (n != raw_len) throw std::runtime_error("zstd: length mismatch");
  return out;
}

Frame encode_compile(const CompileRequest& r, int zstd_level, uint64_t* zstd_len_out) {
  bool raw = zstd_level == kNoCompression;
  std::string z = raw ? std::string() : zstd_compress(r.preprocessed, zstd_level);   // raw: no intermediate copy
  const std::string& body = raw ? r.preprocessed : z;
  if (zstd_len_out) *zstd_len_out = body.size();
  json h{{"id", r.id}, {"argv", r.argv}, {"lang", r.lang}, {"input_name", r.input_name},
         {"raw_len", r.preprocessed.size()}, {"zstd_len", body.size()}};
  if (raw) h["raw"] = true;
  return Frame{Type::Compile, pack(h, body)};
}
CompileRequest decode_compile(const Frame& f, uint64_t* zstd_len_out) {
  json h; std::string z; unpack(f.payload, &h, &z);
  CompileRequest r;
  r.id = h.at("id").get<int64_t>(); r.argv = h.at("argv").get<std::vector<std::string>>();
  r.lang = h.value("lang", "c"); r.input_name = h.value("input_name", "input.c");
  size_t raw_len = h.at("raw_len").get<size_t>();
  if (zstd_len_out) *zstd_len_out = z.size();
  if (h.value("raw", false)) { if (z.size() != raw_len) throw std::runtime_error("compile: raw length mismatch"); r.preprocessed = std::move(z); }
  else r.preprocessed = zstd_decompress(z, raw_len);
  return r;
}
// Compiler stdout/stderr above 4 KiB travel zstd-compressed after the object in the binary blob (classic .i compiles
// of the kernel can emit hundreds of MB of macro-origin warnings per build, too much to send raw and JSON-escaped
// inside the header). Decoders accept both forms.
static constexpr size_t kDiagInline = 4096;
Frame encode_result(const CompileResult& r, int zstd_level) {
  std::string z = r.object.empty() ? std::string() : zstd_compress(r.object, zstd_level);
  bool zdiag = r.out.size() + r.err.size() > kDiagInline;
  std::string zo = zdiag && !r.out.empty() ? zstd_compress(r.out, zstd_level) : std::string();
  std::string ze = zdiag && !r.err.empty() ? zstd_compress(r.err, zstd_level) : std::string();
  json h{{"id", r.id}, {"rc", r.rc}, {"status", r.status}, {"stdout", zdiag ? std::string() : r.out}, {"stderr", zdiag ? std::string() : r.err},
         {"obj_raw_len", r.object.size()}, {"obj_zstd_len", z.size()}, {"queue_s", r.queue_s},
         {"wall_s", r.wall_s}, {"cpu_s", r.cpu_s}, {"in_raw", r.in_raw}, {"in_zstd", r.in_zstd},
         {"mode", r.mode}, {"rounds", r.rounds}, {"miss_bytes", r.miss_bytes}};
  if (!r.depfile.empty()) h["depfile"] = r.depfile;
  if (!r.obj_sha.empty()) h["obj_sha"] = r.obj_sha;
  h["cache"] = r.cache; if (!r.cache_key.empty()) h["cache_key"] = r.cache_key;
  if (r.compile_bench_s > 0) h["compile_bench_s"] = r.compile_bench_s;   // optional: old peers ignore / omit it
  if (!r.compiler_id.empty()) h["compiler_id"] = r.compiler_id;          // optional
  if (!r.sysinc_fp.empty()) h["sysinc_fp"] = r.sysinc_fp;
  if (zdiag) { h["out_raw_len"] = r.out.size(); h["out_zstd_len"] = zo.size(); h["err_raw_len"] = r.err.size(); h["err_zstd_len"] = ze.size(); }
  return Frame{Type::Result, pack(h, zdiag ? z + zo + ze : z)};
}
CompileResult decode_result(const Frame& f) {
  json h; std::string z; unpack(f.payload, &h, &z);
  CompileResult r;
  r.id = h.at("id").get<int64_t>(); r.rc = h.value("rc", -1); r.status = h.value("status", "worker-error");
  r.out = h.value("stdout", ""); r.err = h.value("stderr", "");
  r.queue_s = h.value("queue_s", 0.0); r.wall_s = h.value("wall_s", 0.0); r.cpu_s = h.value("cpu_s", 0.0);
  r.in_raw = h.value("in_raw", 0ULL); r.in_zstd = h.value("in_zstd", 0ULL);
  size_t raw = h.value("obj_raw_len", 0ULL);
  if (h.contains("err_zstd_len")) {   // compressed diagnostics follow the object in the blob
    size_t zl = h.value("obj_zstd_len", 0ULL), ol = h.value("out_zstd_len", 0ULL), el = h.value("err_zstd_len", 0ULL);
    if (zl + ol + el != z.size()) throw std::runtime_error("result: blob length mismatch");
    std::string zo = z.substr(zl, ol), ze = z.substr(zl + ol, el); z.resize(zl);
    if (ol) r.out = zstd_decompress(zo, h.value("out_raw_len", 0ULL));
    if (el) r.err = zstd_decompress(ze, h.value("err_raw_len", 0ULL));
  }
  r.out_raw = raw; r.out_zstd = z.size();
  if (raw) r.object = zstd_decompress(z, raw);
  r.mode = h.value("mode", "classic"); r.depfile = h.value("depfile", ""); r.rounds = h.value("rounds", 0);
  r.miss_bytes = h.value("miss_bytes", 0ULL);
  r.obj_sha = h.value("obj_sha", "");
  r.cache = h.value("cache", "off"); r.cache_key = h.value("cache_key", "");
  r.compile_bench_s = h.value("compile_bench_s", 0.0);
  r.compiler_id = h.value("compiler_id", ""); r.sysinc_fp = h.value("sysinc_fp", "");
  return r;
}

// --- linker domains ---
static json refs_json(const std::vector<FileRef>& v) { json a = json::array(); for (auto& f : v) a.push_back({{"path", f.path}, {"sha", f.sha}, {"size", f.size}}); return a; }
static std::vector<FileRef> refs_from(const json& a) { std::vector<FileRef> v; for (auto& e : a) v.push_back(FileRef{e.value("path", ""), e.value("sha", ""), e.value("size", 0ULL)}); return v; }

Frame encode_prelink(const PrelinkRequest& r) {
  json h{{"id", r.id}, {"domain", r.domain}, {"phase", r.phase}, {"objects", refs_json(r.objects)}, {"roots", r.roots},
         {"options", r.options}, {"live", r.live}};
  return Frame{Type::Prelink, h.dump()};
}
PrelinkRequest decode_prelink(const Frame& f) {
  json h = json::parse(f.payload); PrelinkRequest r;
  r.id = h.at("id").get<int64_t>(); r.domain = h.value("domain", 0); r.phase = h.value("phase", "manifest");
  r.objects = refs_from(h.value("objects", json::array()));
  r.roots = h.value("roots", json::object()); r.options = h.value("options", json::object()); r.live = h.value("live", json::array());
  return r;
}
Frame encode_prelink_reply(const PrelinkReply& r, int zstd_level) {
  std::string z = r.object.empty() ? std::string() : zstd_compress(r.object, zstd_level);
  json h{{"id", r.id}, {"domain", r.domain}, {"phase", r.phase}, {"status", r.status}, {"error", r.error},
         {"manifest", r.manifest}, {"stats", r.stats}, {"raw_len", r.object.size()}, {"zstd_len", z.size()},
         {"wall_s", r.wall_s}, {"cpu_s", r.cpu_s}};
  return Frame{Type::PrelinkResult, pack(h, z)};
}
PrelinkReply decode_prelink_reply(const Frame& f) {
  json h; std::string z; unpack(f.payload, &h, &z); PrelinkReply r;
  r.id = h.at("id").get<int64_t>(); r.domain = h.value("domain", 0); r.phase = h.value("phase", ""); r.status = h.value("status", "error");
  r.error = h.value("error", ""); r.manifest = h.value("manifest", json::object()); r.stats = h.value("stats", json::object());
  r.raw_len = h.value("raw_len", 0ULL); r.zstd_len = z.size(); r.wall_s = h.value("wall_s", 0.0); r.cpu_s = h.value("cpu_s", 0.0);
  if (r.raw_len) r.object = zstd_decompress(z, r.raw_len);
  return r;
}

// ---- header-push frames ---------------------------------------------------------
static json refs_to_json(const std::vector<FileRef>& v) {
  json a = json::array();
  for (auto& f : v) a.push_back({{"path", f.path}, {"sha", f.sha}, {"size", f.size}});
  return a;
}
static std::vector<FileRef> refs_from_json(const json& a) {
  std::vector<FileRef> v;
  for (auto& e : a) v.push_back(FileRef{e.value("path", ""), e.value("sha", ""), e.value("size", 0ULL)});
  return v;
}

Frame encode_compile_hp(const HpRequest& r, int zstd_level, uint64_t* zstd_len_out) {
  std::string z = zstd_compress(r.source, zstd_level);
  if (zstd_len_out) *zstd_len_out = z.size();
  json h{{"id", r.id}, {"argv", r.argv}, {"lang", r.lang}, {"cwd", r.cwd}, {"src_path", r.src_path},
         {"raw_len", r.source.size()}, {"zstd_len", z.size()}, {"deps", refs_to_json(r.deps)}};
  return Frame{Type::CompileHp, pack(h, z)};
}
HpRequest decode_compile_hp(const Frame& f, uint64_t* zstd_len_out) {
  json h; std::string z; unpack(f.payload, &h, &z);
  HpRequest r;
  r.id = h.at("id").get<int64_t>(); r.argv = h.at("argv").get<std::vector<std::string>>();
  r.lang = h.value("lang", "c"); r.cwd = h.value("cwd", "/"); r.src_path = h.value("src_path", "");
  if (zstd_len_out) *zstd_len_out = z.size();
  r.source = zstd_decompress(z, h.at("raw_len").get<size_t>());
  r.deps = refs_from_json(h.value("deps", json::array()));
  return r;
}
Frame encode_cache_put(const CachePutMsg& m, int zstd_level, uint64_t* wire_bytes_out) {
  const std::string z = zstd_compress(m.object, zstd_level == kNoCompression ? 1 : zstd_level);
  json h{{"argv", m.argv}, {"cwd", m.cwd}, {"src_path", m.src_path}, {"src_sha", m.src_sha}, {"deps", refs_to_json(m.deps)},
         {"depfile", m.depfile}, {"stdout", m.out}, {"stderr", m.err}, {"rc", m.rc}, {"obj_sha", m.obj_sha},
         {"raw_len", m.object.size()}, {"zstd_len", z.size()}};
  if (!m.compiler_id.empty()) h["compiler_id"] = m.compiler_id;
  if (!m.sysinc_fp.empty()) h["sysinc_fp"] = m.sysinc_fp;
  Frame f{Type::CachePut, pack(h, z)};
  if (wire_bytes_out) *wire_bytes_out = f.payload.size() + 5;
  return f;
}
CachePutMsg decode_cache_put(const Frame& f) {
  json h; std::string z; unpack(f.payload, &h, &z);
  CachePutMsg m;
  m.argv = h.at("argv").get<std::vector<std::string>>(); m.cwd = h.at("cwd").get<std::string>();
  m.src_path = h.at("src_path").get<std::string>(); m.src_sha = h.at("src_sha").get<std::string>();
  m.deps = refs_from_json(h.value("deps", json::array())); m.depfile = h.value("depfile", "");
  m.out = h.value("stdout", ""); m.err = h.value("stderr", ""); m.rc = h.value("rc", -1); m.obj_sha = h.at("obj_sha").get<std::string>();
  m.compiler_id = h.value("compiler_id", ""); m.sysinc_fp = h.value("sysinc_fp", "");
  if (h.at("zstd_len").get<size_t>() != z.size()) throw std::runtime_error("CACHE_PUT: truncated");
  m.object = zstd_decompress(z, h.at("raw_len").get<size_t>());
  return m;
}

Frame encode_need(const NeedMsg& m) { return Frame{Type::Need, json{{"id", m.id}, {"shas", m.shas}}.dump()}; }
NeedMsg decode_need(const Frame& f) {
  json j = json::parse(f.payload);
  return NeedMsg{j.at("id").get<int64_t>(), j.value("shas", std::vector<std::string>{})};
}
Frame encode_blobs(const BlobsMsg& m, int zstd_level, uint64_t* wire_bytes_out) {
  json blobs = json::array(); std::string data;
  for (auto& [sha, raw] : m.blobs) {
    std::string z = zstd_compress(raw, zstd_level);
    blobs.push_back({{"sha", sha}, {"raw_len", raw.size()}, {"zstd_len", z.size()}});
    data += z;
  }
  if (wire_bytes_out) *wire_bytes_out = data.size();
  json h{{"id", m.id}, {"files", refs_to_json(m.files)}, {"blobs", blobs}};
  return Frame{Type::Blobs, pack(h, data)};
}
Frame encode_prefetch(const BlobsMsg& m, int zstd_level, uint64_t* wire_bytes_out) {
  BlobsMsg c = m; c.id = 0;
  Frame f = encode_blobs(c, zstd_level, wire_bytes_out); f.type = Type::Prefetch; return f;
}
BlobsMsg decode_prefetch(const Frame& f) { Frame g = f; g.type = Type::Blobs; return decode_blobs(g); }

BlobsMsg decode_blobs(const Frame& f) {
  json h; std::string data; unpack(f.payload, &h, &data);
  BlobsMsg m; m.id = h.at("id").get<int64_t>(); m.files = refs_from_json(h.value("files", json::array()));
  size_t off = 0, total = 0;
  for (auto& b : h.value("blobs", json::array())) {
    size_t zl = b.at("zstd_len").get<size_t>(), rl = b.at("raw_len").get<size_t>();
    if (zl > data.size() || off > data.size() - zl) throw std::runtime_error("BLOBS: truncated");
    if (rl > max_blob_bytes() - std::min(total, max_blob_bytes())) throw std::runtime_error("BLOBS: total declared length exceeds OPTICC_MAX_BLOB_MB");
    total += rl;
    m.blobs.emplace_back(b.at("sha").get<std::string>(), zstd_decompress(data.substr(off, zl), rl));
    off += zl;
  }
  return m;
}
Frame encode_missing(const MissingMsg& m) {
  return Frame{Type::Missing, json{{"id", m.id}, {"name", m.name}, {"includer", m.includer}, {"round", m.round}}.dump()};
}
MissingMsg decode_missing(const Frame& f) {
  json j = json::parse(f.payload);
  return MissingMsg{j.at("id").get<int64_t>(), j.value("name", ""), j.value("includer", ""), j.value("round", 0)};
}

}  // namespace opticc::proto

// --- compile cache ---------------------------------------------------------------------------------------------
namespace opticc::proto {
Frame encode_cache_query(const CacheQuery& q) { return {Type::CacheQuery, json{{"keys", q.keys}}.dump()}; }
CacheQuery decode_cache_query(const Frame& f) { CacheQuery q; auto j = json::parse(f.payload); q.keys = j.value("keys", std::vector<std::string>{}); return q; }
Frame encode_cache_have(const CacheHave& h) { std::vector<int> v; v.reserve(h.have.size()); for (bool b : h.have) v.push_back(b ? 1 : 0); return {Type::CacheHave, json{{"have", v}}.dump()}; }
CacheHave decode_cache_have(const Frame& f) { CacheHave h; auto j = json::parse(f.payload); for (int b : j.value("have", std::vector<int>{})) h.have.push_back(b != 0); return h; }
}  // namespace opticc::proto
