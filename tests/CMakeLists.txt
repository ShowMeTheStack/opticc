add_executable(opticc-tests
  test_main.cpp
  test_model.cpp
  test_sim.cpp
  test_protocol.cpp
  test_noise.cpp
  test_cache.cpp
  test_prefetch.cpp
  test_speed.cpp
  test_direct.cpp
  test_rdirect.cpp
  test_scheduler.cpp
  test_hardening.cpp
  test_admission.cpp
  test_cache_safety.cpp
  ${CMAKE_SOURCE_DIR}/src/opticcd/objcache.cpp)
target_include_directories(opticc-tests PRIVATE ${CMAKE_SOURCE_DIR}/src/opticcd)
target_link_libraries(opticc-tests PRIVATE opticc_sim opticc_cacheindex opticc_prefetch opticc_core opticc_warnings)
# direct-hit copy cost: kernel copy vs verified kernel copy vs user-space copy (measurement only)
add_executable(hitpath-bench perf/hitpath_bench.cpp)
target_link_libraries(hitpath-bench PRIVATE opticc_core opticc_warnings)
add_test(NAME bench_hitpath COMMAND hitpath-bench 200)
set_tests_properties(bench_hitpath PROPERTIES TIMEOUT 300 RESOURCE_LOCK opticc_e2e)

set(OPTICC_TEST_NAMES
  sim_prefetch_on_off_legacy
  sim_prefetch_aggregate_warmth
  prefetch_token_bucket
  prefetch_held_set_tracking
  prefetch_popularity_ranking
  json_roundtrip
  noisy_oracle_gamma1
  affine_predictor_learns
  window_formula
  topology_ids_validated
  single_remote_job_time
  fairness_unlimited_client_link
  fairness_shared_egress_cap
  timers_exact
  ect_follows_t1_threshold
  saturated_node_throughput
  window_never_exceeded
  window_blocked_waits_for_best_node
  graham_bound_ect
  distcc_emulation_bytes
  hedge_fires_at_threshold
  simultaneous_attempt_completion
  hedge_runs
  predictor_learning_changes_decisions
  priced_ect_beats_myopic_ect
  header_push_single_job_time
  header_push_store_misses
  auto_mode_prefers_cheaper_client_path
  frame_compile_roundtrip
  frame_result_roundtrip
  pack_unpack_rejects_garbage
  argscan_basic_c
  argscan_cxx_deps
  argscan_x_flag_and_explicit_mf
  argscan_rejects
  sha256_known_vectors
  depfile_parse
  argscan_wp_mmd
  argscan_thinlto_backend
  hp_remote_argv_rewrite
  resolve_include_and_missing_diag
  kat_x25519_rfc7748
  kat_chacha20poly1305_rfc8439
  noise_keys_base64_fingerprint
  noise_handshake_transcript_and_tamper
  noise_psk_challenge
  security_policy_table
  noise_benchmark_runs
  noise_ik_reference_vectors
  incscan_parse_directives
  incscan_closure_small_tree
  incscan_persisted_state_exact
  incscan_synthetic_tree_cost
  tlb_follows_policy_mode
  local_slots_zero_and_localhost_slots
  release_window_jN
  client_overhead_charged
  nu_no_oscillation
  kbuild_cmd_parse
  header_push_store_all_shared
  distcc_pump_runs_header_push
  late_binding_not_worse_than_early
  late_binding_avoids_hol_starvation
  window_never_exceeded_late
  cachekey_stability_and_sensitivity
  objcache_manifest_matching
  objcache_lru_eviction
  objcache_torn_store_is_a_miss
  objcache_two_process_writers_never_tear
  objcache_manifest_replaced_atomically
  content_key_final_newline_and_time_macros
  content_key_comment_insensitive
  content_key_equal_implies_identical_object
  content_key_soundness
  content_key_equal_implies_identical_object_full
  objcache_manifest_fn_matching
  cache_query_roundtrip
  cache_index_holders
  affinity_routes_to_holding_slower_node
  affinity_stale_hint_does_not_skew_placement
  affinity_learned_bloom_precision
  sim_cache_repeat_all_hits
  speed_estimator_recovers_relative_speeds
  speed_estimator_optimism_and_damping
  speed_estimator_client_reference_single_worker
  speed_estimator_prior_refresh_keeps_samples
  compile_result_bench_field_optional
  classic_gate_never_compiles_locally_while_remote_idles
  sim_misjudged_nodes_recover_with_speed_estimation
  result_diagnostics_compressed_roundtrip
  hedging_guard_never_worse_than_no_hedging
  work_conserving_bound_holds_under_misestimated_speeds
  no_workers_with_zero_local_slots_compiles_locally
  incscan_has_include_literal_and_computed
  missing_include_parse_strips_ansi
  speed_estimator_unbiased_under_size_routing
  compile_request_raw_and_compressed_roundtrip
  probe_cache_validation_detects_network_change
  client_work_conservation_header_push_worker_bound
  direct_encode_decode_roundtrip
  direct_positive_stat_then_content
  direct_negative_shadowing
  direct_volatile_directory
  direct_key_sensitivity
  direct_record_lookup
  probe_classify_kbuild_shapes
  sysinc_signatures_and_tracker
  sysinc_follows_symlinks_out_of_the_roots
  sysinc_time_macro_input_per_tu
  probe_premises
  incscan_direct_inputs
  bloom_filter_roundtrip
  direct_safe_args
  direct_keysha_positive
  direct_changed_since_margin
  direct_hash_memo
  argscan_asm_local_cacheable
  rdirect_encode_decode_and_wire
  rdirect_pkey_and_env
  rdirect_two_publishers_one_object_key
  rdirect_validator_content
  rdirect_validator_negatives
  rdirect_store_merge
  rdirect_fingerprint
  rdirect_validator_concurrent_once_and_still_misses
  rdirect_validator_slash_names
  rdirect_validator_missing_dir_negatives
  park_releases_admission_slot
  certain_hits_capped_per_node
  bloom_certainty_wilson_bound
  age_aware_order_for_runtime_jobs
  tu_history_roundtrip
  requeue_lost_rebinds_elsewhere
  nu_reads_observed_occupancy
  client_saturated_signal
  distcc_hp_policy_is_dispatch_only
  i2_core_level_no_queue_while_core_idles
  rebind_local_frees_remote_slot
  work_definition_from_source
  hard_zstd_declared_length_bounded
  hard_blobs_total_bounded
  hard_frame_limit_pre_auth
  hard_decoder_fuzz_bounded
  hard_under_root_contains_hostile_paths
  hard_safe_file_stem
  hard_servescope_rule
  hard_frame_id_of_malformed_request
  hard_timed_waits
  admission_work_conserving_beyond_npp
  i2_helpers_in_isolation
  holder_trust_regime_change
  holder_trust_reset_on_readmission
  cache_index_held_only_from_worker_result
  rdirect_key_kinds
  safe_args_refuses_extra_outputs
  compiler_identity_covers_helpers
  keeps_comments_sanitizers_openmp
  file_directive_detector
  compiler_identity_single_flight)
foreach(t ${OPTICC_TEST_NAMES})
  add_test(NAME ${t} COMMAND opticc-tests ${t} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# End-to-end on loopback: real opticcd + coordinator + client driving make.
add_test(NAME e2e_loopback COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e.sh)
set_tests_properties(e2e_loopback PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_LOCAL_CACHE=off;OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 300)
# the same loopback build with early binding must also be byte-identical
add_test(NAME e2e_loopback_early COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e.sh)
set_tests_properties(e2e_loopback_early PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_BINDING=early;OPTICC_LOCAL_CACHE=off;OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 300)
add_test(NAME e2e_header_push COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_hp.sh)
set_tests_properties(e2e_header_push PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_LOCAL_CACHE=off;OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 600)
add_test(NAME e2e_hedge_and_window COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_hedge.sh)
set_tests_properties(e2e_hedge_and_window PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_LOCAL_CACHE=off;OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 300)
add_test(NAME e2e_security COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_sec.sh)
set_tests_properties(e2e_security PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_LOCAL_CACHE=off;OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 900)

add_subdirectory(elf)
add_subdirectory(perf)
# header prefetch: piggybacked and background header pushes, objects byte-identical
add_test(NAME e2e_prefetch COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_prefetch.sh)
set_tests_properties(e2e_prefetch PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_LOCAL_CACHE=off;OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 600)
# worker compile cache: hits on rebuild, exact invalidation on header edit, classic-mode cache
add_test(NAME e2e_compile_cache COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache.sh)
set_tests_properties(e2e_compile_cache PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_LOCAL_CACHE=off;OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 300)

# client-local object cache end to end (its own cache dir; repeat build answered locally)
add_test(NAME e2e_client_cache COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_clientcache.sh)
set_tests_properties(e2e_client_cache PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 300)

# probe-cache validation (stale rtt/bandwidth from another network must be re-measured)
add_test(NAME e2e_probe_cache COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_probecache.sh)
set_tests_properties(e2e_probe_cache PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 120)

# a worker down at coordinator start-up is retried and admitted during the build
add_test(NAME e2e_late_worker COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_lateworker.sh)
set_tests_properties(e2e_late_worker PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# the compiler wrapper's direct cache path (byte-identical objects/dep files through edits, shadowing, flag changes)
add_test(NAME e2e_direct_cache COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_direct.sh)
set_tests_properties(e2e_direct_cache PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 600)
# assembler compiles cached; regenerated-identical inputs hit; regenerated-different miss; .incbin never cached
add_test(NAME e2e_asm_regen COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_asm_regen.sh)
set_tests_properties(e2e_asm_regen PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR};OPTICC_PROBE_CACHE=${CMAKE_BINARY_DIR}/test-probe-cache.json" TIMEOUT 300)
# client-local results written back to the worker cache (CACHE_PUT); a fresh client hits them there
add_test(NAME e2e_writeback COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_writeback.sh)
# shared direct manifests: a fresh client is answered from another client's direct entries, never after a relevant change
add_test(NAME e2e_rdirect COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_rdirect.sh)
# guards on the local preprocessed-text key (__has_include changes, __TIME__, __builtin_COLUMN, -P, .incbin)
add_test(NAME e2e_pp_key_hardening COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_pp_key_hardening.sh)
set_tests_properties(e2e_pp_key_hardening PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# an edit of a file read only through an inline-asm .incbin reaches the object on every cache path
add_test(NAME e2e_incbin_all_paths COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_incbin_all_paths.sh)
set_tests_properties(e2e_incbin_all_paths PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a header edited while its compile runs (local and remote) never yields a cache entry keyed on the edited content
add_test(NAME e2e_store_race COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_store_race.sh)
set_tests_properties(e2e_store_race PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# TUs with a computed __has_include are cached under the hash of their preprocessed text
add_test(NAME e2e_pp_key COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_pp_key.sh)
set_tests_properties(e2e_pp_key PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# inputs generated by the build right before their compile are cached; writes during the compile refuse the store
add_test(NAME e2e_gen_input COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_gen_input.sh)
set_tests_properties(e2e_gen_input PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# kbuild-style compiler probes answered by the wrapper's probe cache (no coordinator needed)
add_test(NAME e2e_cc_probes COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cc_probes.sh)
set_tests_properties(e2e_cc_probes PROPERTIES ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
set_tests_properties(e2e_writeback PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
set_tests_properties(e2e_rdirect PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# worker-cache affinity only for the exact closure content (no stale holder after a header edit)
add_test(NAME e2e_affinity_fullkey COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_affinity_fullkey.sh)
set_tests_properties(e2e_affinity_fullkey PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a fresh client (empty caches and memo, new coordinator) probes every TU with its full content key
add_test(NAME e2e_affinity_fresh COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_affinity_fresh.sh)
set_tests_properties(e2e_affinity_fresh PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 600)
# a worker obtains through MISSING only files inside the job's scope (never the client's keys)
add_test(NAME e2e_hardening_scope COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_hardening_scope.sh)
set_tests_properties(e2e_hardening_scope PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# malformed requests are answered; silent workers, hanging compilers and a stuck coordinator end in a
# local fallback (attempt deadline, compile watchdog, wrapper timeout)
add_test(NAME e2e_hardening_timeouts COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_hardening_timeouts.sh)
set_tests_properties(e2e_hardening_timeouts PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 400)
# the worker applies the pre-auth frame limit and the BW probe cap on a live connection
add_test(NAME e2e_hardening_bounds COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_hardening_bounds.sh)
set_tests_properties(e2e_hardening_bounds PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 120)
# a MISSING-round input is part of the closure a worker-cache entry is filed under
add_test(NAME e2e_cache_missing COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_missing.sh)
set_tests_properties(e2e_cache_missing PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# no dependency file + computed include -> the client cache must not store
add_test(NAME e2e_cache_nomd COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_nomd.sh)
set_tests_properties(e2e_cache_nomd PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# the write-back vouches only for closure hashes re-taken after the compile
add_test(NAME e2e_cache_upload_closure COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_upload_closure.sh)
set_tests_properties(e2e_cache_upload_closure PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a remote result is cached only when the worker's toolchain identity equals the client's
add_test(NAME e2e_cache_toolchain COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_toolchain.sh)
set_tests_properties(e2e_cache_toolchain PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a /usr header outside the compiler's search list is an ordinary (keyed) input; skipped (77) unless root
add_test(NAME e2e_cache_usrinc COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_usrinc.sh)
set_tests_properties(e2e_cache_usrinc PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300 SKIP_RETURN_CODE 77)
# flags reading unkeyed files (ignore-lists, profiles, plugins, ...) are never cached on any path
add_test(NAME e2e_cache_unsafe_flag COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_unsafe_flag.sh)
set_tests_properties(e2e_cache_unsafe_flag PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a torn cache entry (short object, missing meta) is a miss on the direct path and the client cache
add_test(NAME e2e_cache_torn_store COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_torn_store.sh)
set_tests_properties(e2e_cache_torn_store PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# an include-affecting environment (CPATH) keys the caches and keeps the compile on the client
add_test(NAME e2e_cache_env COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_env.sh)
set_tests_properties(e2e_cache_env PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# path spellings (__FILE__) and, with -g, the compile directory are keyed
add_test(NAME e2e_cache_paths COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_paths.sh)
set_tests_properties(e2e_cache_paths PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a TU expanding __DATE__/__TIME__/__TIMESTAMP__ is never served from a content-keyed cache
add_test(NAME e2e_cache_time_macros COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_time_macros.sh)
set_tests_properties(e2e_cache_time_macros PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a classic compile with debug info is not cached by the worker (its key has no compile directory)
add_test(NAME e2e_cache_classic_g COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_classic_g.sh)
set_tests_properties(e2e_cache_classic_g PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a time macro reached through a fingerprinted system header refuses every content-keyed store
add_test(NAME e2e_cache_sys_time_macros COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_sys_time_macros.sh)
# a header found through a project's -isystem directory (omitted from -MMD output) is still keyed
add_test(NAME e2e_cache_mmd_isystem COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_mmd_isystem.sh)
set_tests_properties(e2e_cache_mmd_isystem PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a SIGUSR1 summary waiting for store decisions does not stall the coordinator's watcher loop
add_test(NAME e2e_cache_usr1_nonblocking COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_usr1_nonblocking.sh)
set_tests_properties(e2e_cache_usr1_nonblocking PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 120)
# a header-push job that turns local does not build its closure a second time
add_test(NAME e2e_cache_late_local_closure COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_cache_late_local_closure.sh)
set_tests_properties(e2e_cache_late_local_closure PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
set_tests_properties(e2e_cache_sys_time_macros PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300 SKIP_RETURN_CODE 77)
# the write-back hashes before it stats; an edit in between refuses the upload
add_test(NAME e2e_upload_race COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_upload_race.sh)
set_tests_properties(e2e_upload_race PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# an upload compiled by a different compiler binary is refused
add_test(NAME e2e_upload_identity COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_upload_identity.sh)
set_tests_properties(e2e_upload_identity PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# no compiler output under the comment-insensitive key; '/*' in a comment forces the raw hash
add_test(NAME e2e_diag_key COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_diag_key.sh)
set_tests_properties(e2e_diag_key PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# a worker whose compile benchmark lands after the coordinator probed it has its microbenchmark prior refreshed once
add_test(NAME e2e_late_bench COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_late_bench.sh)
set_tests_properties(e2e_late_bench PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300)
# gcc through opticc (header-push, classic, client cache), byte-identical to plain gcc
add_test(NAME e2e_gcc COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_e2e_gcc.sh)
set_tests_properties(e2e_gcc PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300 SKIP_RETURN_CODE 77)
# the direct-cache benchmark's worker must not outlive a SIGKILLed benchmark
add_test(NAME bench_direct_bench_killed COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/run_direct_bench_killed.sh)
set_tests_properties(bench_direct_bench_killed PROPERTIES RESOURCE_LOCK opticc_e2e ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 300 SKIP_RETURN_CODE 77)
# Test daemon hygiene. Every e2e/bench test runs with OPTICC_EXIT_WITH_PARENT=1 (an opticcd it starts exits when the
# test script dies, e.g. on a ctest timeout), and all of them require the fixture whose cleanup test
# e2e_no_orphan_listeners FAILS if any opticcd from this build tree is still running afterwards (and terminates it).
get_property(_opticc_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _opticc_tests)
  if(_t MATCHES "^(e2e_|bench_)")
    set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT "OPTICC_EXIT_WITH_PARENT=1")
    set_property(TEST ${_t} APPEND PROPERTY FIXTURES_REQUIRED opticc_daemons)
  endif()
endforeach()
add_test(NAME e2e_no_orphan_listeners COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/check_no_orphans.sh)
set_tests_properties(e2e_no_orphan_listeners PROPERTIES FIXTURES_CLEANUP opticc_daemons
                     ENVIRONMENT "OPTICC_BUILD=${CMAKE_BINARY_DIR}" TIMEOUT 60)
