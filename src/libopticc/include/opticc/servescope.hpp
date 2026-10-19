// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// Which client files a worker may obtain through a MISSING round.
//
// A worker names a missing include (`name`, `includer`); the client resolves it and ships the file. If an
// absolute name were resolved unchecked, a compromised worker could ask for any file the client could read (e.g. the
// client's Noise private key in ~/.config/opticc) and then impersonate the client to every other worker.
//
// Rule (checked on the CANONICAL path: symlinks resolved, so a link inside the project cannot point at a secret):
//   1. the file is a regular file;
//   2. it is not under a denied directory: the opticc key directory (OPTICC_KEY_DIR, ~/.config/opticc), ~/.ssh,
//      ~/.gnupg, /etc/ssh, /proc, /sys, /dev, and any extra directory the caller passes (the coordinator's cache);
//      /etc/shadow and /etc/gshadow are denied as files;
//   3. it lies within the job's scope: the job's cwd, the source file's directory, every include directory of the
//      command (-iquote, -I, -isystem, -idirafter), the directory of every forced include (-include/-imacros), the
//      directory of the includer when the includer is already one of this job's files, and the PROJECT ROOTS — the
//      deepest common ancestor of the source directory and each of those directories (this covers quote includes such
//      as "../x.h" that climb out of the includer's directory); an ancestor equal to "/" is never a root.
// Files outside the scope are refused: the worker's compile then fails and the client compiles locally (correct).
#include <string>
#include <vector>

namespace opticc::servescope {

struct Scope {
  std::vector<std::string> allow;   // canonical directories
  std::vector<std::string> deny;    // canonical directories
};

// Build the scope of one job. `includer_ok` is the includer named by the worker if (and only if) it is already one of
// the job's files, else "". `extra_deny` are additional directories to deny (e.g. the coordinator's cache directory).
Scope build(const std::vector<std::string>& argv, const std::string& cwd, const std::string& src,
            const std::string& includer_ok, const std::vector<std::string>& extra_deny);

// True when `path` may be served; *why gets the reason when not.
bool allowed(const Scope& s, const std::string& path, std::string* why = nullptr);

// The canonical form of `p` (symlinks resolved) when it exists, else its lexical normal form.
std::string canonical_or_lexical(const std::string& p);
// Deepest common ancestor of two absolute normal paths, by whole components ("/a/bc", "/a/b" -> "/a"); "/" if none.
std::string common_ancestor(const std::string& a, const std::string& b);

}  // namespace opticc::servescope
