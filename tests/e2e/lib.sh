# Shared test hygiene for e2e/perf scripts. Source it right after `set -u`:
#   . "$(dirname "$0")/lib.sh"            (tests/e2e)   or   . "$(dirname "$0")/../e2e/lib.sh"   (tests/perf)
# * OPTICC_EXIT_WITH_PARENT=1: every opticcd started from this script exits when the script dies, even when ctest kills
#   the script on a timeout (its EXIT trap never runs) or a non-interactive shell ignores Ctrl-C.
# * Where `setsid` exists (Linux), the script re-executes itself as the leader of a fresh process group; on exit every
#   other member of that group (background daemons, helpers) is sent SIGTERM. A script that installs its own EXIT trap
#   after sourcing this file replaces this one; OPTICC_EXIT_WITH_PARENT and the ctest guard e2e_no_orphan_listeners
#   still apply.
export OPTICC_EXIT_WITH_PARENT=1
if [ -z "${E2E_REGROUPED:-}" ] && command -v setsid >/dev/null 2>&1 \
   && [ "$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')" != "$$" ]; then
  export E2E_REGROUPED=1; exec setsid --wait bash "$0" "$@"
fi
e2e_kill_group() {
  local pg; pg=$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')
  if [ -n "$pg" ] && [ "$pg" = "$$" ]; then trap '' TERM; kill -TERM -- -"$pg" 2>/dev/null; fi
  true
}
trap 'rc=$?; e2e_kill_group; exit $rc' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
