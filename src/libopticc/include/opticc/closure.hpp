// opticc: header-push dependency closure helpers.
// Used by the coordinator: parse Makefile-style dependency files, query the
// compiler's default (system) include directories, resolve a missing include
// the way the preprocessor would.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string>
#include <vector>

#include "opticc/argscan.hpp"

namespace opticc::closure {

// Dependencies listed in a .d file (targets excluded, "\ " unescaped, order kept, duplicates removed).
std::vector<std::string> parse_depfile(const std::string& text);

// Dependencies recorded by kbuild in `dir/.foo.o.cmd` (fixdep output, kbuild >= 5.x):
//   savedcmd_dir/foo.o := <command>
//   source_dir/foo.o := /abs/or/rel/foo.c
//   deps_dir/foo.o := //     include/linux/kconfig.h //     $(wildcard include/config/FOO) \      <- config-symbol pseudo-deps, skipped
//     ...
//   dir/foo.o: $(deps_dir/foo.o)
//   $(deps_dir/foo.o):
// Returns the deps_ list (order kept, duplicates removed, $(wildcard ...) entries dropped), with the
// source file first when a source_ line is present. Paths are relative to the kbuild output dir (cwd).
std::vector<std::string> parse_kbuild_cmd(const std::string& text);

// `dir/.foo.o.cmd` for output `dir/foo.o` ("" if out has no basename).
std::string kbuild_cmd_path(const std::string& out);

// The compiler's default include search directories for `lang` ("c" | "c++"),
// from `<compiler> -E -v -x <lang> /dev/null` run in `cwd`. Empty on failure.
// Headers under these directories exist on every worker image and are not shipped.
std::vector<std::string> system_include_dirs(const std::string& compiler, const std::string& lang, const std::string& cwd);

// Resolve `name` as the preprocessor would for an #include in `includer`
// (absolute path): the includer's directory, -iquote, -I, -isystem, -idirafter
// (all made absolute against cwd). Returns "" when not found.
std::string resolve_include(const std::string& name, const std::string& includer, const args::IncludeDirs& dirs,
                            const std::vector<std::string>& system_dirs, const std::string& cwd);

// Parse a compiler "missing include" diagnostic. clang: "<file>:<l>:<c>: fatal error: 'x.h' file not found";
// gcc: "<file>:<l>:<c>: fatal error: x.h: No such file or directory". Returns false if none.
bool parse_missing_include(const std::string& stderr_text, std::string* name, std::string* includer);

std::string absolutize(const std::string& path, const std::string& cwd);

}  // namespace opticc::closure
