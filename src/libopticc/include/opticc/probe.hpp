// opticc: client-side cache for COMPILER PROBES (kbuild cc-option / as-option / as-instr / try-run, autoconf-style
// feature tests, `cc --version`).
//
// Problem: while make parses the kbuild
// makefiles it runs ~350 probe compiles (`$(CC) -Werror <flags> <flag> -c -x c /dev/null -o "$TMP"`,
// `printf ... | $(CC) ... -c -x assembler-with-cpp -o "$TMP" -`, `$(CC) --version`, `-dM -E -x c /dev/null`) inside
// $(shell ...), i.e. SERIALLY on the wall-clock path. Passing each one to the compiler costs
// about 12 ms; ccache answers most of them from its cache in about a millisecond. This cache answers them in the
// wrapper.
//
// What is cached: an invocation whose ONLY input is /dev/null or stdin ("-"), in one of the modes -c, -S, -E, or a
// pure information query (`--version`, `-dumpversion`, `-dumpfullversion`, `-dumpmachine` as the only argument).
// The cached RESULT is (exit status, stdout bytes, stderr bytes, output-file bytes): failures are cached too
// (an unsupported flag is the common case for cc-option), which ccache does not do.
//
// Key  P = sha256(format, compiler identity (resolved path + dev, ino, size, mtime_ns, ctime_ns), configuration-file
//          identity (every *.cfg next to the resolved compiler and in /etc/clang, with its stat), mode, argv exactly as
//          given with the OUTPUT PATH replaced by a placeholder, cwd, environment that can change the compiler's
//          behaviour or messages (direct::relevant_env + locale + CLANG_NO_DEFAULT_CONFIG), sha256 of the input bytes)
//
// Correctness argument. A compiler run is a deterministic function of the compiler
// installation, its configuration files, its arguments, its environment, the cwd and the files it reads. For the
// cached shapes the files it reads are exactly: the input (/dev/null = empty, or the stdin bytes, hashed into P), and
// files named by the arguments or the input. classify() refuses every argument that makes the compiler READ a file
// (-include, -imacros, @response files, plugins, profiles, sanitizer lists, spec files, -B, --config, modules/PCH,
// assembler @files) or WRITE a file other than the -o output (dependency files, -save-temps, split DWARF, time traces,
// stack-usage, coverage notes, optimization records, assembler listings, dumps); stdin content that could name a file
// (#include/#import/#embed/__has_include, .include/.incbin — any occurrence of "include", "import", "incbin",
// "embed") is not cached. What remains read is the installation (pinned by the identity, as in every opticc and
// ccache cache: compiler_check=mtime) and its configuration files (pinned by their stats). The output path does not
// influence the output bytes for these shapes (no split DWARF / dependency targets / listings are allowed, and the
// object's file symbol and debug name come from the INPUT name, which is in argv). Therefore equal P implies an equal
// (rc, stdout, stderr, output). Determinism exceptions are excluded at store time: a result is stored only if the
// compiler exited normally with rc 0 (and produced its output) or rc 1 with a non-empty diagnostic that names no
// environmental cause (I/O errors, missing directories, permissions, resource exhaustion, crashes — see
// cacheable_failure()); __DATE__/__TIME__ cannot occur in an empty or include-free input; colour codes depend on a
// terminal, so nothing is cached or replayed when stdout or stderr is a terminal.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string>
#include <vector>

namespace opticc::probe {

enum class Mode { None, Compile, Assemble, Preprocess, Info };

struct Shape {
  bool ok = false;
  bool looks_like_probe = false;   // the input is /dev/null or stdin (reported in traces when !ok)
  std::string why;                 // why not cacheable (when !ok)
  Mode mode = Mode::None;
  bool stdin_input = false;        // input "-"
  int input_index = -1;
  int output_index = -1;           // argv index holding the output (the value after -o, or the joined -oPATH token)
  bool output_joined = false;      // -oPATH
  std::string output;              // as written; "" none (stdout for -E), "-" stdout, "/dev/null" discarded
};

// argv[0] = the real compiler (path as resolved on PATH).
Shape classify(const std::vector<std::string>& argv);

// Stats of every compiler configuration file the driver could load: *.cfg next to the resolved executable and in
// /etc/clang (clang's default search; user/system config dirs set by environment are part of probe::env()).
std::string config_identity(const std::string& resolved_compiler);

// direct::relevant_env() plus the locale (GCC localises diagnostics) and CLANG_NO_DEFAULT_CONFIG.
std::vector<std::string> env();

std::string key(const std::string& compiler_identity, const std::string& config_identity, const std::vector<std::string>& argv,
                const Shape& s, const std::string& cwd, const std::vector<std::string>& env, const std::string& input_sha);

// stdin content that could make the compiler or assembler read another file (conservative substring test).
bool input_reads_files(const std::string& content);

struct Result { int rc = 0; std::string out, err; bool has_obj = false; std::string obj; };
std::string encode(const Result& r);
bool decode(const std::string& blob, Result* r);

// Identity of the files the driver reads IMPLICITLY for language `lang` (glibc's
// stdc-predef.h, which both clang and GCC include without being asked), found once per compiler and language with
// `cc -x LANG -M /dev/null` and memoised under memo_dir; returns the hash of their stats and of the stats of the system
// include directories (a newly created implicit file changes its directory). "" = unknown (do not cache).
std::string implicit_identity(const std::string& resolved_compiler, const std::string& lang, const std::string& memo_dir);
std::string lang_of(const std::vector<std::string>& argv);   // the -x language ("c" if none)
// Does the driver run an EXTERNAL assembler for a -c in this language (GCC: always; clang: with
// -fno-integrated-as or for some targets)? From the driver's own -### plan; memoised; unknown = true (not cached).
bool external_assembler(const std::string& resolved_compiler, const std::string& lang, const std::string& memo_dir);

// rc 1 with a non-empty diagnostic that names no environmental cause (see the header comment).
bool cacheable_failure(const std::string& err);

std::string entry_path(const std::string& cache_dir, const std::string& pkey);   // <cache>/p/<xx>/<key>

}  // namespace opticc::probe
