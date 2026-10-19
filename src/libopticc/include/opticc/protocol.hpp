// opticc: worker wire protocol.
//
// Frame:   u32 big-endian length N  |  u8 type  |  N-1 bytes payload
// Payload: for JSON-only frames, UTF-8 JSON text. For frames that carry a
//          blob: u32 big-endian header length H | H bytes JSON header | blob.
//          Blobs are zstd-compressed (level params.zstd_level, default 3);
//          the header records raw_len and zstd_len so rho can be measured.
//
// Types (client -> worker unless noted):
//   HELLO        JSON {token, version, client}             -> HELLO_ACK JSON {cores, mem_bytes, bench_s, version, hostname}
//   PING         empty                                     -> PONG empty (answered immediately by the reader thread)
//   BW           JSON header {down_bytes} + raw blob (not compressed) -> BW_ACK header {up_bytes, recv_s} + down_bytes of zeros
//   COMPILE      header {id, argv[], lang, input_name, raw_len, zstd_len} + zstd(preprocessed source)
//                -> RESULT header {id, rc, stdout, stderr, obj_raw_len, obj_zstd_len, queue_s, wall_s, cpu_s, status}
//                   + zstd(object) ; status: "ok" | "compile-error" | "cancelled" | "worker-error"
//   CANCEL       JSON {id}                                 (no reply; the RESULT, if any, has status "cancelled")
//   BYE          empty
// Header-push mode (the worker preprocesses):
//   COMPILE_HP   header {id, argv[], lang, cwd, src_path, raw_len, zstd_len, deps:[{path, sha, size}]} + zstd(source)
//                deps are the absolute paths of the non-system headers the client believes the TU needs
//                (from an existing .d file; may be empty on a first build).
//   NEED         JSON {id, shas[]}            (worker -> client) content the worker's store lacks
//   BLOBS        header {id, files:[{path, sha, size}], blobs:[{sha, raw_len, zstd_len}]} + concatenated zstd blobs
//                (client -> worker) store content and, via `files`, extra path->sha mappings to materialise
//   MISSING      JSON {id, name, includer, round}  (worker -> client) the compile failed on a missing include;
//                the client resolves it and answers with BLOBS (possibly empty files[] = "cannot resolve")
//   RESULT       as above plus {mode, depfile, rounds, miss_bytes}; depfile is the -MD/-MMD output with the
//                worker's job root stripped, so the client writes it exactly as a local compile would have.
// One connection carries any number of pipelined COMPILE requests; results may
// arrive in any order and are matched by id (the per-node window relies on this FIFO pipeline).
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace opticc::proto {

using json = nlohmann::json;
constexpr int kVersion = 1;
constexpr int kDefaultPort = 3633;
// Compile requests may travel UNCOMPRESSED ("raw" flag) when the link has ample bandwidth -- zstd -3
// over ~3 MB kernel .i files costs the (bottleneck) client ~9 ms/job; encode_compile with this level sends raw bytes.
constexpr int kNoCompression = -100000;

enum class Type : uint8_t {
  Hello = 1, HelloAck = 2, Ping = 3, Pong = 4, Bw = 5, BwAck = 6,
  Compile = 10, Result = 11, Cancel = 12, Bye = 13,
  CompileHp = 14, Need = 15, Blobs = 16, Missing = 17,
  // linker domains
  Prelink = 20, PrelinkResult = 21,
  // distributed compile cache
  CacheQuery = 22, CacheHave = 23,
  // header prefetch
  Prefetch = 24,
  // write-back of a client-local compile result into the worker's compile cache
  CachePut = 25,
  // shared direct manifests (opticc/rdirect.hpp): publish, fetch, reply
  DirectPut = 26, DirectGet = 27, DirectReply = 28,
};

struct Frame {
  Type type = Type::Bye;
  std::string payload;
};

// Blob framing helpers (header JSON + blob).
std::string pack(const json& header, const std::string& blob);
void unpack(const std::string& payload, json* header, std::string* blob);  // throws on malformed input
// The request id of a frame whose full decode failed (header-framed or plain JSON payload), or -1;
// lets a worker answer a malformed request with a RESULT instead of leaving the client waiting. Never throws.
int64_t frame_id(const Frame& f);

// zstd wrappers. decompress needs the raw length (carried in the header).
std::string zstd_compress(const std::string& raw, int level);
std::string zstd_decompress(const std::string& z, size_t raw_len);
// Every size a peer declares is bounded BEFORE memory is allocated for it. One blob (an object,
// a preprocessed source, a header, a manifest) may not exceed max_blob_bytes() = OPTICC_MAX_BLOB_MB (default 512 MiB:
// the largest real payloads are LLVM .i files of tens of MB); zstd_decompress also requires the frame's own recorded
// content size, when present, to equal the declared length, so a forged raw_len cannot force a large allocation.
size_t max_blob_bytes();

// Big-endian u32 helpers.
void put_u32(std::string& out, uint32_t v);
uint32_t get_u32(const char* p);

struct CompileRequest {
  int64_t id = 0;
  std::vector<std::string> argv;   // original client argv (compiler first); rewritten on the worker
  std::string lang;                // "c" | "c++"
  std::string input_name;          // e.g. "foo.c" (worker derives .i/.ii name)
  std::string preprocessed;        // raw bytes
};
struct CompileResult {
  int64_t id = 0;
  int rc = -1;
  std::string status;              // see header comment
  std::string out, err;            // compiler stdout / stderr
  std::string object;              // raw object bytes (empty on failure)
  double queue_s = 0, wall_s = 0, cpu_s = 0;
  uint64_t in_raw = 0, in_zstd = 0, out_raw = 0, out_zstd = 0;
  // header-push extras
  std::string mode = "classic";    // "classic" | "header_push"
  std::string depfile;             // -MD/-MMD output (root-stripped), empty if none
  int rounds = 0;                  // MISSING round trips
  uint64_t miss_bytes = 0;         // raw bytes received via BLOBS for this job
  std::string obj_sha;             // sha256 of the object when the worker kept it in its store (--keep-objects)
  // compile cache: "off" (worker has no cache) | "miss" | "store" (miss, now stored) | "hit"
  std::string cache = "off";
  std::string cache_key;           // manifest key M (header-push) or classic key (classic); for the coordinator's index
  // The worker's compile benchmark (s) once known; 0 = not yet measured / old worker (field absent). opticcd listens
  // before its benchmark finishes, so the hello ack may carry 0; the first result carrying it refreshes the prior.
  double compile_bench_s = 0;
  // The identity of the toolchain that produced `object` on the worker — compiler id (binary + version)
  // and the content fingerprint of its system include tree. The client caches a remote result only when both equal
  // its own (empty = old worker: never cached). Optional fields: old peers omit / ignore them.
  std::string compiler_id, sysinc_fp;
};



struct FileRef { std::string path; std::string sha; uint64_t size = 0; };

struct HpRequest {
  int64_t id = 0;
  std::vector<std::string> argv;   // original client argv (compiler first); rewritten on the worker
  std::string lang;
  std::string cwd;                 // client working directory (absolute)
  std::string src_path;            // absolute path of the source on the client
  std::string source;              // raw source bytes
  std::vector<FileRef> deps;       // known closure (absolute paths)
};
struct NeedMsg { int64_t id = 0; std::vector<std::string> shas; };
struct BlobsMsg {
  int64_t id = 0;
  std::vector<FileRef> files;                               // path -> sha mappings to materialise
  std::vector<std::pair<std::string, std::string>> blobs;   // (sha, raw bytes)
};
struct MissingMsg { int64_t id = 0; std::string name, includer; int round = 0; };
// PREFETCH  same payload as BLOBS with id 0 (client -> worker, no reply): content-addressed header bytes pushed into
//           the worker's store ahead of the jobs that need them. The worker verifies sha256(raw) == sha before
//           storing (a corrupt or forged blob is dropped, never served). Frames are kept <= ~1 MiB raw so the worker
//           stores them inline in its reader thread: a PREFETCH followed by a COMPILE_HP on the same connection is
//           therefore always stored before that job looks at its store (FIFO guarantee, no NEED round).
Frame encode_prefetch(const BlobsMsg& m, int zstd_level, uint64_t* wire_bytes_out = nullptr);
BlobsMsg decode_prefetch(const Frame& f);

// CACHE_PUT (client -> worker, no reply): header {argv, cwd, src_path, src_sha, deps:[{path, sha, size}], depfile,
//   stdout, stderr, rc, obj_sha, raw_len, zstd_len} + zstd(object). The result of a compile the CLIENT ran, described
//   exactly as a COMPILE_HP of the same TU would be (same argv, cwd, source path, shipped closure), so the worker
//   files it under the keys its own compile would have produced (cachekey manifest/object keys) and a later
//   COMPILE_HP from any client hits it. The worker verifies sha256(object) == obj_sha and stores nothing on any
//   mismatch or doubt; it refuses all uploads when started with --no-cache-uploads.
struct CachePutMsg {
  std::vector<std::string> argv;
  std::string cwd, src_path, src_sha;   // src_sha: raw sha256 of the source bytes compiled
  std::vector<FileRef> deps;            // the closure a COMPILE_HP of this TU ships (paths absolute, raw sha256)
  std::string depfile;                  // the compile's dependency file text ("" if none)
  std::string out, err; int rc = 0;
  std::string object, obj_sha;
  // The key the worker files the object under names the WORKER's compiler and system
  // headers; the object came from the CLIENT's. The worker stores an upload only when both identities are equal:
  // compiler_id = cachekey::compiler_id of the compiler the client ran (binary sha256 + version line), sysinc_fp =
  // sysinc content fingerprint of that compiler's system include directories. Absent (an older client) = refused.
  std::string compiler_id, sysinc_fp;
};
Frame encode_cache_put(const CachePutMsg& m, int zstd_level, uint64_t* wire_bytes_out = nullptr);
CachePutMsg decode_cache_put(const Frame& f);

Frame encode_compile_hp(const HpRequest& r, int zstd_level, uint64_t* zstd_len_out = nullptr);
HpRequest decode_compile_hp(const Frame& f, uint64_t* zstd_len_out = nullptr);
Frame encode_need(const NeedMsg& m);
NeedMsg decode_need(const Frame& f);
Frame encode_blobs(const BlobsMsg& m, int zstd_level, uint64_t* wire_bytes_out = nullptr);
BlobsMsg decode_blobs(const Frame& f);
Frame encode_missing(const MissingMsg& m);
MissingMsg decode_missing(const Frame& f);

// --- linker domains -----------------------------------------------------------------------------------------
// PRELINK  header {id, domain, phase:"manifest"|"prelink", objects:[{path, sha, size}] (domain order = link order),
//                  roots:{entry:[...], keep:[...]}, live:[[obj,sec],...] (phase prelink), options:{gc, apply_pcrel,
//                  keep_function_sections, fold_data, static_link, exports:[...]}}        (no blob)
//          The worker loads the objects from its content store (NEED/BLOBS fetch any it lacks, as in header-push).
//          phase "manifest": replies with the condensed domain manifest + exports/imports.
//          phase "prelink":  expands the live set from the live entries, runs the domain prelink and replies with the
//                            relocatable domain object.
// PRELINK_RESULT header {id, domain, phase, status:"ok"|"error", error, manifest (json), stats (json), raw_len,
//                        zstd_len, wall_s, cpu_s} + zstd(domain object)   (blob empty for phase manifest)
struct PrelinkRequest {
  int64_t id = 0; int domain = 0; std::string phase;
  std::vector<FileRef> objects;
  json roots = json::object(), options = json::object();
  json live = json::array();
};
struct PrelinkReply {
  int64_t id = 0; int domain = 0; std::string phase, status = "error", error;
  json manifest = json::object(), stats = json::object();
  std::string object;              // raw bytes (phase prelink)
  double wall_s = 0, cpu_s = 0; uint64_t raw_len = 0, zstd_len = 0;
};
Frame encode_prelink(const PrelinkRequest& r);
PrelinkRequest decode_prelink(const Frame& f);
Frame encode_prelink_reply(const PrelinkReply& r, int zstd_level);
PrelinkReply decode_prelink_reply(const Frame& f);

// --- compile cache -----------------------------------------------------------------------------------------
// CACHE_QUERY JSON {keys:[M...]} (client -> worker)  ->  CACHE_HAVE JSON {have:[0|1...]} answered by the worker's
// reader thread from its in-memory manifest index (no disk, no slot). Lets the coordinator route a job to the
// worker that already holds its object (cache affinity in ECT).
struct CacheQuery { std::vector<std::string> keys; };
struct CacheHave { std::vector<bool> have; };
Frame encode_cache_query(const CacheQuery& q);
CacheQuery decode_cache_query(const Frame& f);
Frame encode_cache_have(const CacheHave& h);
CacheHave decode_cache_have(const Frame& f);

Frame encode_compile(const CompileRequest& r, int zstd_level, uint64_t* zstd_len_out = nullptr);
CompileRequest decode_compile(const Frame& f, uint64_t* zstd_len_out = nullptr);
Frame encode_result(const CompileResult& r, int zstd_level);
CompileResult decode_result(const Frame& f);

}  // namespace opticc::proto
