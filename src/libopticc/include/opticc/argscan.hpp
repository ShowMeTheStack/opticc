// opticc: compiler command-line classification and rewriting. Rules follow
// distcc's dcc_scan_args: only "compile one source with -c to one -o object"
// is distributable; everything else runs locally.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string>
#include <vector>

namespace opticc::args {

struct Scan {
  bool distributable = false;
  std::string reason;          // why not, when !distributable
  // An assembler compile (.S/.s, -x assembler[-with-cpp]) with -c and one source: never distributed, but its
  // result is cacheable on the client (direct path + client cache), as ccache caches it. lang is then "asm".
  bool local_cacheable = false;
  std::string compiler;        // argv[0]
  std::string source;          // as written on the command line
  std::string output;          // -o value (as written); empty = default <src>.o
  std::string lang;            // "c" | "c++" | "asm"
  bool has_md = false;         // -MD/-MMD present (dep file produced during preprocessing), incl. -Wp,-MD,x forms
  bool has_mf = false, has_mt = false;
  std::string dep_file;        // -MF value (or -Wp,-MMD,<file>) as written; empty = compiler default <out-stem>.d
  std::string dep_target;      // -MT/-MQ value as written; empty = compiler default (the -o value)
  int source_index = -1, output_index = -1;  // positions in argv
  // distributed ThinLTO backend job (`clang -c -x ir <module.o> -fthinlto-index=<module.o.thinlto.bc> -o <native.o>`,
  // lang "ir"): the index file as written; its sibling <stem>.imports lists the bitcode modules the
  // backend imports from. Such jobs run only in header-push mode (nothing to preprocess); their closure is the index
  // plus the imported modules.
  std::string thinlto_index;
};

Scan scan(const std::vector<std::string>& argv);

// Build the local preprocessing command: original argv minus -c/-o, plus
// "-E -o <ifile>"; when -MD/-MMD is present without -MF/-MT the dep file and
// target are pinned to what the real compile would have produced.
std::vector<std::string> preprocess_argv(const std::vector<std::string>& argv, const Scan& s, const std::string& ifile);

// Build the remote command: strip preprocessor-only flags (-I -D -U -include
// -isystem -iquote -idirafter -MD -MMD -MF -MT -MQ -MP -Wp,* -H), replace the
// source with the preprocessed file and -o with the remote object path, and
// turn "-x c" into "-x cpp-output" (etc.) so the compiler does not re-preprocess.
std::vector<std::string> remote_argv(const std::vector<std::string>& argv, const Scan& s,
                                     const std::string& ifile, const std::string& ofile);

// Include search directories as written on the command line, in order
// (header-push MISSING resolution; relative entries are resolved by the caller against cwd).
struct IncludeDirs {
  std::vector<std::string> quote;      // -iquote
  std::vector<std::string> bracket;    // -I
  std::vector<std::string> system;     // -isystem
  std::vector<std::string> after;      // -idirafter
  std::vector<std::string> forced;     // -include / -imacros files
};
IncludeDirs include_dirs(const std::vector<std::string>& argv);

// Command that prints the non-system dependency closure (`-MM`) to stdout:
// original argv minus -c/-o and any -M* / -Wp,-M* flags.
std::vector<std::string> mm_argv(const std::vector<std::string>& argv, const Scan& s);

// Header-push remote command (worker side). Files are materialised under
// `root` at their absolute client paths and the compiler runs with cwd =
// root/<client cwd>; absolute path-bearing flags and an absolute source are
// prefixed with root, relative ones are left alone, -o points at `ofile`,
// -MF/-MT/-MQ/-Wp,-M* are replaced by "-MF depfile -MT dep_target" when the
// client asked for dependency output, and "-ffile-prefix-map=root=" keeps
// __FILE__, DWARF paths and the dep file identical to a local compile.
std::vector<std::string> hp_remote_argv(const std::vector<std::string>& argv, const Scan& s, const std::string& root,
                                        const std::string& ofile, const std::string& depfile, const std::string& dep_target);
// Default dependency-file name the compiler would use for -MD/-MMD with this -o.
std::string default_dep_name(const std::string& output);

// Local compile command (fallback): argv unchanged; here for symmetry/tests.
std::string default_object_name(const std::string& source);
std::string preprocessed_suffix(const std::string& lang);   // ".i" | ".ii"

}  // namespace opticc::args
