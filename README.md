# opticc

opticc is a distributed compiler for C and C++ builds. Like distcc, it sends
compile jobs from one machine (the *client*) to other machines (the *workers*)
and returns the object files, so that `make -j`, ninja or any other build system
runs faster without changes to the build itself. Unlike distcc, opticc

* preprocesses on the workers by default: the client ships the source and only
  the headers a worker does not already hold, so the client's CPU is not the
  bottleneck;
* decides for every translation unit whether to compile it locally or remotely,
  and on which worker, from measured round-trip time, bandwidth and speed of
  each worker, and keeps a per-worker number of jobs in flight sized to the
  link;
* keeps a compile cache on the client and on every worker, shared by all
  clients, with strict rules about what may be reused;
* talks to workers only over an authenticated, encrypted channel (Noise IK on
  libsodium) unless plaintext is explicitly allowed on a private network.

It is licensed under the GNU General Public License, version 2 (see `LICENSE`).

## Contents

1. [Requirements](#requirements)
2. [Building](#building)
3. [Installing](#installing)
4. [Programs](#programs)
5. [Quick start](#quick-start)
6. [Setting up a worker](#setting-up-a-worker)
7. [Setting up a client](#setting-up-a-client)
8. [Using opticc with a build](#using-opticc-with-a-build)
9. [How a compile is handled](#how-a-compile-is-handled)
10. [Caches](#caches)
11. [Security](#security)
12. [Linking](#linking)
13. [Configuration reference](#configuration-reference)
14. [Monitoring and diagnostics](#monitoring-and-diagnostics)
15. [The simulator](#the-simulator)
16. [Running the tests](#running-the-tests)
17. [Troubleshooting](#troubleshooting)

## Requirements

* A C++20 compiler (tested with GCC 14 and clang 20).
* CMake 3.25 or newer and Ninja (make also works).
* libsodium 1.0.18 or newer.
* libzstd. If it is not installed, CMake downloads and builds zstd 1.5.6.
* POSIX threads.

Workers run Linux. Clients have been used on Linux and macOS. Every worker must
have the same compilers (same versions, same paths, same system headers) as the
clients that use it: opticc ships sources and project headers, never system
headers or compilers. The linker-domain tools handle x86-64 ELF only.

Packages:

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build pkg-config libsodium-dev libzstd-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config libsodium-devel libzstd-devel

# macOS (Homebrew)
brew install cmake ninja pkg-config libsodium zstd
```

## Building

```sh
git clone <repository-url> opticc
cd opticc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `OPTICC_BUILD_TESTS` | `ON` | build the unit and end-to-end tests |
| `OPTICC_WERROR` | `OFF` | treat compiler warnings as errors |

The programs are written to `build/src/opticc/opticc`, `build/src/opticcd/opticcd`,
`build/src/sim/` and `build/src/tools/`.

## Installing

There is no install target. Copy the two programs you need onto each machine,
for example:

```sh
sudo install -m 0755 build/src/opticc/opticc   /usr/local/bin/opticc     # clients
sudo install -m 0755 build/src/opticcd/opticcd /usr/local/bin/opticcd    # workers
```

Build the programs on (or for) the same operating system and C library as the
machines that run them.

## Programs

| Program | Runs on | Purpose |
|---|---|---|
| `opticc` | client | compiler wrapper; also the per-user coordinator (`opticc --coordinator`), key generation (`opticc keygen`) and a few utilities |
| `opticcd` | worker | the worker daemon: accepts authenticated connections, compiles jobs, keeps the header store and the worker cache |
| `opticc-sim` | anywhere | discrete-event simulator that runs the same scheduler on a synthetic or recorded trace |
| `opticc-gentrace` | anywhere | generates synthetic compile traces for the simulator |
| `opticc-prelink` | anywhere | ELF tool behind linker domains: manifests, liveness, partial links |
| `opticc-domains` | anywhere | partitions an object reference graph into linker domains |
| `opticc-rdirect-bench` | client | measures the cost of shared direct-manifest lookups for a `compile_commands.json` |

## Quick start

Two machines: a worker at `192.168.1.20` and a client. Both have the same
compilers installed.

On the client, create the client key and copy the public key line it prints:

```sh
opticc keygen
# created client key in /home/you/.config/opticc
#   public key:  <base64 public key>
#   fingerprint: SHA256:...
```

On the worker, authorize that key and start the daemon:

```sh
sudo mkdir -p /etc/opticc
echo '<base64 client public key>' | sudo tee -a /etc/opticc/authorized_clients
opticcd --listen 192.168.1.20:3633 --allow 192.168.1.0/24 --jobs 16
# opticcd prints its own public key (pubkey=...) and fingerprint on start-up
```

On the client, point opticc at the worker (with the worker's public key) and
build:

```sh
export OPTICC_HOSTS='192.168.1.20:3633/16#<base64 worker public key>'
make -j48 CC='opticc gcc' CXX='opticc g++'
```

That is all. The first `opticc` call starts a coordinator in the background;
it measures the worker, schedules every compile and stops by itself after 30
minutes without work.

## Setting up a worker

### Starting opticcd

```sh
opticcd [--listen IP:PORT] [--allow CIDR[,CIDR...]] [--jobs N] [--tmp DIR]
        [--token T] [--zstd LEVEL] [--cas-gb G]
        [--object-cache DIR] [--object-cache-gb N] [--no-cache-uploads] [--keep-objects]
        [--key-dir DIR] [--authorized-clients FILE] [--accept noise|plain|both] [--psk FILE]
        [--exit-with-parent] [-v]
```

| Option | Default | Meaning |
|---|---|---|
| `--listen IP:PORT` | `127.0.0.1:3633` | address to listen on; give the worker's private address |
| `--allow CIDR[,CIDR]` | `127.0.0.1/32` | client addresses allowed to connect; checked before anything else |
| `--jobs N` | number of online cores | concurrent compiles |
| `--tmp DIR` | `$TMPDIR` | parent of the per-job working directories |
| `--token T` | none | shared token that clients must also send (`OPTICC_TOKEN`) |
| `--zstd LEVEL` | `3` | compression level for returned objects |
| `--cas-gb G` | `2` | size of the header store (content-addressed, least recently used entries evicted) |
| `--object-cache DIR` | off | enable the worker compile cache in `DIR` |
| `--object-cache-gb N` | `8` | size of the worker compile cache |
| `--no-cache-uploads` | uploads accepted | refuse objects that clients compiled locally and offer to the worker cache |
| `--keep-objects` | off | keep per-job directories after a compile (debugging) |
| `--key-dir DIR` | `/etc/opticc` if writable, else `~/.config/opticc` | where `host.key` and `host.pub` live |
| `--authorized-clients FILE` | `<key-dir>/authorized_clients` | client public keys allowed to connect |
| `--accept noise\|plain\|both` | `noise` | which channels the worker accepts |
| `--psk FILE` | none | pre-shared key required from plaintext clients |
| `--exit-with-parent` | off | exit when the parent process exits |
| `-v` | off | log every job |

On first start, opticcd creates its key pair (`host.key`, `host.pub`) in the
key directory and prints the public key and its fingerprint. Give the public
key to the clients (see below).

`authorized_clients` holds one base64 client public key per line; lines
starting with `#` are comments. The file is read again for every connection, so
keys can be added or removed without restarting the daemon.

### Running opticcd as a service

Run the daemon as an unprivileged user and keep it on a private network. A
systemd unit:

```ini
# /etc/systemd/system/opticcd.service
[Unit]
Description=opticc worker
After=network-online.target

[Service]
User=opticc
Group=opticc
ExecStart=/usr/local/bin/opticcd --listen 192.168.1.20:3633 --allow 192.168.1.0/24 \
          --key-dir /etc/opticc --object-cache /var/cache/opticcd --object-cache-gb 20
Restart=on-failure
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

```sh
sudo useradd --system --home /var/cache/opticcd opticc
sudo mkdir -p /etc/opticc /var/cache/opticcd
sudo chown opticc:opticc /etc/opticc /var/cache/opticcd
sudo systemctl daemon-reload
sudo systemctl enable --now opticcd
journalctl -u opticcd | grep pubkey     # the worker's public key
```

Open the worker port (3633 by default) only to the clients' network.

### What a worker needs

* The same compilers as the clients, at the same paths, with the same system
  headers. opticc runs the compiler named in the job; it never ships
  compilers.
* Enough disk for the header store (`--cas-gb`) and, if enabled, the worker
  cache (`--object-cache-gb`).

## Setting up a client

### Keys

```sh
opticc keygen            # creates ~/.config/opticc/client.key and client.pub
opticc keygen --dir DIR  # elsewhere (or set OPTICC_KEY_DIR)
```

Running it again shows the existing key. Add the printed public key to every
worker's `authorized_clients`.

### Listing the workers

The coordinator reads the worker list from `OPTICC_HOSTS`: entries separated by
spaces or commas, each of the form

```
host[:port][/cores][@speed][#pubkey][+plain]
```

| Part | Meaning |
|---|---|
| `host` | name or address of the worker |
| `:port` | port, default 3633 |
| `/cores` | number of concurrent compiles to plan for; default: what the worker reports |
| `@speed` | relative speed hint; the coordinator measures speed and refines it during the build |
| `#pubkey` | the worker's base64 public key; pins the worker |
| `+plain` | allow an unencrypted connection to this worker (private addresses only, see [Security](#security)) |

Example:

```sh
export OPTICC_HOSTS='10.0.0.11:3633/16#Qm9...= 10.0.0.12/8#ZXh...= 10.0.0.13#cGw...='
```

Worker keys can also be pinned in `~/.config/opticc/known_workers`, one
`host:port <base64 public key>` per line. With `OPTICC_TOFU=1`, the first
connection to an unknown worker records its key there, as ssh does with host
keys; this is off by default.

An empty `OPTICC_HOSTS` compiles everything locally (the client caches still
apply). `OPTICC_HOSTS=__local__` bypasses opticc entirely: every compile runs
the real compiler directly.

## Using opticc with a build

Put `opticc` in front of the compiler:

```sh
make -j48 CC='opticc gcc' CXX='opticc g++'
make -j48 CC='opticc clang' CXX='opticc clang++'
```

With CMake:

```sh
cmake -S . -B build -G Ninja \
      -DCMAKE_C_COMPILER_LAUNCHER=opticc -DCMAKE_CXX_COMPILER_LAUNCHER=opticc
ninja -C build -j48
```

With the Linux kernel:

```sh
make -j48 CC='opticc clang' LLVM=1        # or CC='opticc gcc'
```

Alternatively, use opticc as a masquerade: create symbolic links named after
the compilers in a directory that comes first in `PATH`. opticc then finds the
real compiler further down `PATH`, skipping itself:

```sh
mkdir -p ~/opticc-bin
for c in cc c++ gcc g++ clang clang++; do ln -sf /usr/local/bin/opticc ~/opticc-bin/$c; done
PATH=~/opticc-bin:$PATH make -j48
```

### Choosing `-j`

Use a parallelism of about the total number of worker cores plus the client's
cores. The coordinator decides how many jobs actually run on each worker and
keeps the rest queued, so a higher `-j` does not overload the workers.

### The coordinator

The first `opticc` call of a user starts the coordinator (`opticc
--coordinator`) on the Unix socket `/tmp/opticc-<uid>/coord.sock` (set
`OPTICC_SOCK` to change it). All concurrent compiles of that user share it. It
connects to the workers, measures their round-trip time and bandwidth, reads
each worker's compile benchmark, and then makes every scheduling decision. It
exits after `OPTICC_IDLE_TIMEOUT` seconds without work (default 1800). To start
it explicitly with other settings:

```sh
opticc --coordinator --hosts "$OPTICC_HOSTS" --policy ect-window \
       --summary build-summary.json --idle-timeout 600 -v
```

Settings are read when the coordinator starts. After changing `OPTICC_HOSTS`
or another coordinator setting, stop the running coordinator (or wait for its
idle timeout) so that a new one picks them up:

```sh
pkill -u "$USER" -f 'opticc --coordinator'
```

## How a compile is handled

For every compiler invocation, opticc:

1. Decides whether the command can be distributed. Only a compile of one
   source file with `-c` is distributed. Links, preprocess-only runs and
   unusual commands run locally, unchanged.
2. Answers repeated compiler probes (such as the `-v`, `--version` and
   `-print-*` queries that configure scripts and the kernel's build system run
   many times) from the probe cache.
3. Looks the compile up in the client cache (see [Caches](#caches)). A hit
   copies the object and the dependency file without contacting the
   coordinator.
4. Otherwise hands the compile to the coordinator, which may answer it from a
   shared manifest held by the workers, or schedules it on a client core or a
   worker.
5. On a worker, the job runs in its own directory. The object and the
   dependency file come back byte-identical to those of a local compile; paths
   in them refer to the client's directories.
6. If anything fails remotely (network, worker, timeout), the job is compiled
   locally. A build never fails because of opticc.

### Preprocessing modes

`OPTICC_MODE` selects where preprocessing happens:

| Mode | What happens |
|---|---|
| `header-push` (default) | the client scans the source's includes, sends the source and the digests of its headers, and sends only the headers the worker's store lacks; the worker preprocesses and compiles |
| `classic` | the client preprocesses and sends the preprocessed text, as distcc does |
| `auto` | the scheduler chooses per job and per worker |

In header-push mode, includes that are computed at preprocessing time are
resolved by asking the worker which headers it is missing; system headers are
never sent.

### Scheduling

`OPTICC_POLICY` selects the scheduler:

| Policy | Behaviour |
|---|---|
| `ect-window` (default) | earliest predicted completion time, with a per-worker in-flight window sized to the link |
| `ect` | earliest predicted completion time with one slot per core |
| `distcc` | fixed slots per worker, as distcc does |
| `ect-window-hedge`, `ect-window-hedge-t7` | `ect-window` plus duplication of jobs that run unusually long |

The default binds a job to a worker only when the job is ready to send
(`OPTICC_BINDING=late`). Idle client cores also take jobs.

## Caches

opticc has two cache levels and a probe cache.

**Client cache.** Results of compiles made by this user, stored under
`~/.cache/opticc/objects` (5 GB by default). Hits are answered by the wrapper
itself from a *direct manifest*: the list of files the compile read, with their
sizes, modification and change times, and content hashes. A hit requires every
listed file to be unchanged.

**Worker cache.** Enabled with `opticcd --object-cache`. It is shared by every
client of the worker. Its keys cover the compiler identity, the normalized
command line, the source and every header the compile read. Workers also hold
*shared direct manifests*, so that a new client, or a client after a clean
checkout, can reuse objects that another client produced.

**Probe cache.** Answers repeated compiler probes; entries expire after 10
minutes (`OPTICC_PROBE_TTL_S`).

Results are never stored when reuse could be wrong. In particular, opticc does
not store a compile whose output or diagnostics depend on more than the files
it read: sources or headers that use `__DATE__`, `__TIME__` or
`__TIMESTAMP__`; assembler `.incbin` or `.include` directives (which read
files outside the include mechanism); command lines with unsupported or
output-producing flags; and compiles whose inputs changed while they ran. The
caches assume a local file system for the source tree.

To disable the client cache, set `OPTICC_LOCAL_CACHE=off`. To clear it, delete
`~/.cache/opticc` while no coordinator is running.

## Security

A distributed compiler executes compilers on behalf of its clients, so access
to a worker must be limited to trusted clients.

**Channel.** By default every connection uses the Noise protocol
`Noise_IK_25519_ChaChaPoly_BLAKE2b` (libsodium): client and worker authenticate
each other with static keys, traffic is encrypted, and there is no negotiation
that could be downgraded. A client connects to a worker only if it knows the
worker's public key (`#pubkey`, `known_workers`, or trust on first use when
`OPTICC_TOFU=1`).

**Plaintext.** Unencrypted connections are possible only when the client asks
for them (`OPTICC_SECURITY=plain` or `+plain` on the host), the worker accepts
them (`--accept plain` or `both`), and the worker's address is private
(loopback, RFC 1918, link-local, carrier-grade NAT, or a subnet of a local
VPN interface). A pre-shared key (`--psk FILE` on the worker,
`OPTICC_PSK_FILE` on the client) can protect plaintext workers. Plaintext to a
public address is refused.

**Worker hardening.** Every size a peer declares is bounded before memory is
allocated; paths sent by a client are confined to the job's directory; files a
worker asks the client for are served only if they are regular files inside
the build's scope and outside a deny list (opticc's keys, `~/.ssh`, caches,
`/proc`, `/sys`, `/dev`, `/etc/ssh`); and every request, compile and connection
has a timeout. Compiles run with the worker user's permissions and without a
sandbox: an authorized client can run any compiler and flags on the worker, so
authorize only clients you trust and run `opticcd` as an unprivileged user.

**Verification.** `OPTICC_VERIFY=0.02` compiles 2% of remote jobs a second time
on another worker and compares the results. On a mismatch the job is compiled
locally to decide, a worker that returned a wrong object is excluded for the
rest of the build, and the objects it delivered are recompiled locally. This
requires deterministic compiles.

## Linking

For large links, opticc can partially link groups of objects (*domains*) on
the workers that compiled them, so that the final link has less work:

```sh
export OPTICC_LINK_DOMAINS=1
make -j48 CC='opticc clang'          # links that go through the wrapper use domains
opticc --link -- ld.lld <arguments>  # or call the link explicitly
```

The final link is identical in its allocated sections, symbol addresses and
code to a link of the original objects; if anything fails, the original link
command runs. Domains apply to x86-64 ELF links of at least 128 MB of input
(`OPTICC_LINK_MIN_MB`).

## Configuration reference

All settings are environment variables read by the coordinator when it starts,
or by the wrapper on every call.

### Coordinator

| Variable | Default | Meaning |
|---|---|---|
| `OPTICC_HOSTS` | empty | worker list (see [Listing the workers](#listing-the-workers)) |
| `OPTICC_MODE` | `header-push` | `header-push`, `classic` or `auto` |
| `OPTICC_POLICY` | `ect-window` | scheduler (see [Scheduling](#scheduling)) |
| `OPTICC_BINDING` | `late` | `late` or `early` binding of jobs to workers |
| `OPTICC_LOCAL_SLOTS` | `-1` | compile slots on the client; `-1` means all cores |
| `OPTICC_IDLE_TIMEOUT` | `1800` | seconds without work before the coordinator exits |
| `OPTICC_SECURITY` | `auto` | `auto` (Noise when the key is known), `noise` (never plaintext) or `plain` |
| `OPTICC_TOFU` | off | `1` records unknown worker keys on first contact |
| `OPTICC_KEY_DIR` | `~/.config/opticc` | client key and `known_workers` |
| `OPTICC_PSK_FILE` | none | pre-shared key for plaintext workers |
| `OPTICC_TOKEN` | none | token sent to workers started with `--token` |
| `OPTICC_VERIFY` | `0` | fraction of remote jobs compiled twice and compared |
| `OPTICC_ZSTD` | adaptive | fixed zstd level for shipped data |
| `OPTICC_PRICED` | `on` | charge each choice for the client CPU it uses; `off` disables |
| `OPTICC_SPEED_EST` | `on` | refine worker speeds online from completed compiles |
| `OPTICC_PREFETCH` | `1` | push popular headers to workers ahead of need |
| `OPTICC_PREFETCH_WARM_MB` | `8` | headers pushed to each worker at connection time |
| `OPTICC_PREFETCH_BUDGET_MB` | `64` | total prefetch per worker once the build shows remote demand |
| `OPTICC_LOCAL_CACHE` | `on` | the client cache |
| `OPTICC_LOCAL_CACHE_DIR` | `~/.cache/opticc/objects` | client cache directory |
| `OPTICC_LOCAL_CACHE_GB` | `5` | client cache size |
| `OPTICC_UPLOAD` | `on` | offer locally compiled objects to the worker cache |
| `OPTICC_RDIRECT` | `on` | look up shared direct manifests on the workers |
| `OPTICC_RDIRECT_PUBLISH` | `on` | publish this client's manifests to the workers |
| `OPTICC_PROBE_CACHE` | on | the compiler-probe cache |
| `OPTICC_PROBE_TTL_S` | `600` | lifetime of a cached probe answer |
| `OPTICC_ATTEMPT_TIMEOUT_S` | `3600` | limit for one remote attempt before a local fallback |
| `OPTICC_SUMMARY` | none | write a JSON summary of the build to this file |
| `OPTICC_LOG` | `/dev/null` | coordinator log file |
| `OPTICC_VERBOSE` | off | `1` logs every job |
| `OPTICC_LINK_DOMAINS` | `0` | `1` enables linker domains |
| `OPTICC_LINK_MIN_MB` | `128` | smallest link input that uses domains |

### Wrapper

| Variable | Default | Meaning |
|---|---|---|
| `OPTICC_SOCK` | `/tmp/opticc-<uid>/coord.sock` | coordinator socket (keep the path short) |
| `OPTICC_DIRECT` | `on` | answer client-cache hits in the wrapper |
| `OPTICC_CC_PROBES` | `on` | the compiler-probe cache |
| `OPTICC_WRAPPER_TIMEOUT_S` | `7200` | limit after which the wrapper compiles locally |

### Worker

| Variable | Default | Meaning |
|---|---|---|
| `OPTICC_KEY_DIR` | see `--key-dir` | key directory if `--key-dir` is not given |
| `OPTICC_COMPILE_TIMEOUT_S` | `1800` | limit for one compile |
| `TMPDIR` | `/tmp` | header store and scratch space |

## Monitoring and diagnostics

* `OPTICC_SUMMARY=build-summary.json` writes, when the coordinator exits, a
  JSON summary: the measured round-trip time, bandwidth and speed of every
  worker, the in-flight windows, every job with its predicted and measured
  work and where it ran, bytes shipped, cache hits and misses, and fallbacks.
  Send the coordinator `SIGUSR1` to write the summary during a build:

  ```sh
  pkill -USR1 -u "$USER" -f 'opticc --coordinator'
  ```

* `OPTICC_LOG=/tmp/opticc.log OPTICC_VERBOSE=1` logs every scheduling decision.
* `opticcd -v` logs every job on the worker.
* `opticc --bench-crypto` measures the encryption throughput of the client.
* `opticc cache-report <compiler> [source tree...]` prints diagnostics of the
  system-header fingerprint that the cache keys use for that compiler.

## The simulator

The simulator runs the same scheduler code on a trace, with simulated time, so
that policies can be compared without a cluster:

```sh
build/src/sim/opticc-gentrace --n 2500 --seed 1 --out /tmp/trace.json
build/src/sim/opticc-sim --nodes examples/nodes-phet.json --trace /tmp/trace.json \
    --policy ect-window --rtt-ms 80 --bw-gbps 1 --out /tmp/result.json --lp /tmp/lp.json
```

`examples/` contains node descriptions for homogeneous and heterogeneous pools.
`opticc-sim` without arguments prints all options. `--lp` writes the linear
program of the makespan lower bound, which any LP solver can solve.

## Running the tests

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure -j4
```

The unit tests cover the scheduler, the wire protocol, the Noise channel
(against known-answer vectors), the cache keys and store rules, argument
handling and the ELF tools. The end-to-end tests start real `opticcd` workers
on loopback, build small projects with clang (and GCC, if installed) through
opticc, and check that objects and dependency files are byte-identical to a
plain build, that every cache refuses what it must refuse, and that failures
fall back to local compiles. They need clang, and take a few minutes.

## Troubleshooting

* **Everything compiles locally.** Check `OPTICC_HOSTS`, that the worker's
  public key is given (`#pubkey`) or known, and that the client's key is in the
  worker's `authorized_clients`. Start the coordinator by hand with `-v` to see
  connection errors. Remember that a running coordinator keeps its old
  settings until it exits.
* **A plaintext connection is refused.** Plaintext is allowed only to private
  addresses and only when the worker accepts it; use the Noise channel (give
  the worker's key) instead.
* **Objects differ from a local build.** The worker's compiler or system
  headers differ from the client's. Install identical versions at identical
  paths.
* **A worker is never used.** It may be too slow or too far away for the jobs
  to pay off: the coordinator ships a job only when it predicts an earlier
  finish than a local compile. The build summary shows the measured speed and
  round-trip time of every worker.
* **Stale results after changing compilers.** The cache keys include the
  compiler identity, so a changed compiler causes misses, not wrong hits. To
  reclaim space, delete `~/.cache/opticc` and the worker's cache directory.
