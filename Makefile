CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
BUILD := build/host
# Where the pinned Wine checkout lives for the release-evidence target: the
# conventional ignored location, overridable with
# `make wine-check WINE_SOURCE=<checkout>`.
WINE_SOURCE ?= .deps/wine/source
HEADERS := $(wildcard include/*.h src/*.h native/*.h tests/*.h)

.PHONY: all test wine-check sanitize audit check-whitespace inspect inspect-only sample native native-release box86-catalog clean

all: test
	$(MAKE) audit check-whitespace

$(BUILD):
	mkdir -p $@

define test_rule
$(if $(strip $(3)),\
$(BUILD)/$(1): $(2) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $$(filter %.c %.S,$$^) $(3) -o $$@,\
$(BUILD)/$(1): $$(addprefix $(BUILD)/obj/shared/,$$(addsuffix .o,$$(basename $(2)))) | $(BUILD)
	$$(CC) $$(CFLAGS) $$^ -o $$@)
endef

# Compile common test and tool sources once, with compiler-emitted header
# dependencies so unrelated header edits do not rebuild every executable.
SHARED_SOURCES := $(wildcard src/*.c tests/*.c native/*.c tools/*.c \
	src/*.S tests/*.S native/*.S tools/*.S)
SHARED_DEPFILES := $(addprefix $(BUILD)/obj/shared/,$(addsuffix .d,$(basename $(SHARED_SOURCES))))
-include $(SHARED_DEPFILES)

$(BUILD)/obj/shared/%.o: %.c | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD)/obj/shared/%.o: %.S | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

CORE := src/pe_image.c src/pe_layout.c src/pe_reloc.c src/pe_import.c \
	src/pw_map.c src/pw_module_name.c src/pw_vm.c src/pw_vm_posix.c \
	src/pw_loader.c src/pw_result.c src/pw_segment.c src/pw_compat32.c

# Every source the bounded Wine gate is built from. The service adapters used
# to be part of pw_wine_gate.c, so this list was written out in eight rules and
# would drift the moment one of them was not updated; it lives here once.
WINE_GATE := src/pw_wine_gate.c src/pw_guest_vm.c src/pw_guest_process.c \
	src/pw_nt_handle.c src/pw_unix_call.c src/pw_sha256.c src/pw_export.c \
	src/pe_export.c src/pe_tls.c src/pw_import_bind.c src/pw_x86_engine.c \
	src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c \
	src/pw_guest_call.c src/pw_wine_path.c src/pw_wine_handle.c \
	src/pw_wine_file.c src/pw_wine_registry.c src/pw_wine_query.c \
	src/pw_wine_section.c src/pw_wine_object.c src/pw_wine_thread.c \
	src/pw_unixlib.c \
	src/pw_wine_unixlib.c src/pw_wine_runner.c

$(eval $(call test_rule,test_pe_image,tests/test_pe_image.c src/pe_image.c src/pw_result.c,))
$(eval $(call test_rule,test_pe_resource,tests/test_pe_resource.c src/pe_resource.c src/pe_image.c,))
$(eval $(call test_rule,test_pw_guest_heap,tests/test_pw_guest_heap.c src/pw_guest_heap.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_registry,tests/test_pw_registry.c src/pw_registry.c,))
$(eval $(call test_rule,test_pw_registry_store,tests/test_pw_registry_store.c src/pw_registry_store.c src/pw_registry.c,))
$(eval $(call test_rule,test_pw_ini,tests/test_pw_ini.c src/pw_ini.c,))
$(eval $(call test_rule,test_pw_app_profile,tests/test_pw_app_profile.c src/pw_app_profile.c,))
$(eval $(call test_rule,test_pw_profile_catalog,tests/test_pw_profile_catalog.c src/pw_profile_catalog.c,))
$(eval $(call test_rule,test_pw_prefix,tests/test_pw_prefix.c src/pw_prefix.c,))
$(eval $(call test_rule,test_pw_prefix_ps5,tests/test_pw_prefix_ps5.c native/pw_prefix_ps5.c src/pw_prefix.c,-DPW_PREFIX_PS5_HOST_TEST))
$(eval $(call test_rule,test_pw_profile_session_flow,tests/test_pw_profile_session_flow.c native/pw_prefix_ps5.c src/pw_prefix.c src/pw_app_profile.c src/pw_registry_store.c src/pw_registry.c src/pw_runtime_supervisor.c,-DPW_PREFIX_PS5_HOST_TEST))
$(eval $(call test_rule,test_pw_runtime_supervisor,tests/test_pw_runtime_supervisor.c src/pw_runtime_supervisor.c src/pw_app_profile.c src/pw_prefix.c,))
$(eval $(call test_rule,test_launcher_model,tests/test_launcher_model.c src/pw_launcher_model.c src/pw_runtime_supervisor.c src/pw_app_profile.c src/pw_ini.c src/pw_prefix.c,))
$(eval $(call test_rule,test_prefix_launcher,tests/test_prefix_launcher.c src/pw_prefix_registry.c src/pw_launcher_model.c src/pw_runtime_supervisor.c src/pw_app_profile.c src/pw_ini.c src/pw_prefix.c,))
$(eval $(call test_rule,test_pw_gdi,tests/test_pw_gdi.c src/pw_gdi.c,))
$(eval $(call test_rule,test_pw_present,tests/test_pw_present.c src/pw_present.c src/pw_gdi.c,))
$(eval $(call test_rule,test_pw_wine_heap,tests/test_pw_wine_heap.c wine/ps5/pw_wine_heap.c,-pthread))
$(eval $(call test_rule,test_pw_wine_prx,tests/test_pw_wine_prx.c wine/ps5/pw_wine_prx.c,-I.))
$(eval $(call test_rule,test_pw_launcher_render,tests/test_pw_launcher_render.c src/pw_launcher_render.c,))
$(eval $(call test_rule,test_pw_gdi_abi,tests/test_pw_gdi_abi.c src/pw_win32.c src/pw_user32.c src/pw_gdi.c src/pw_crt_format.c src/pw_registry.c src/pw_guest_heap.c src/pw_guest_args.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_module_name.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_crt_format,tests/test_pw_crt_format.c src/pw_crt_format.c,))
$(eval $(call test_rule,test_pw_user32,tests/test_pw_user32.c src/pw_user32.c,))
$(eval $(call test_rule,test_pw_pad,tests/test_pw_pad.c src/pw_pad.c src/pw_user32.c,))
$(eval $(call test_rule,test_pe_layout,tests/test_pe_layout.c src/pe_image.c src/pe_layout.c src/pw_result.c,))
$(eval $(call test_rule,test_pe_reloc,tests/test_pe_reloc.c src/pe_image.c src/pe_reloc.c src/pw_result.c,))
$(eval $(call test_rule,test_pe_import,tests/test_pe_import.c src/pe_image.c src/pe_import.c src/pw_result.c,))
$(eval $(call test_rule,test_pe_export,tests/test_pe_export.c src/pe_image.c src/pe_export.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_module_name,tests/test_pw_module_name.c src/pw_module_name.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_file_posix,tests/test_pw_file_posix.c src/pw_file_posix.c src/pw_module_name.c,))
$(eval $(call test_rule,test_pw_file_ps5,tests/test_pw_file_ps5.c native/pw_file_ps5.c,))
$(eval $(call test_rule,test_pw_vm,tests/test_pw_vm.c src/pw_vm.c src/pw_vm_posix.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_map,tests/test_pw_map.c $(CORE),))
$(eval $(call test_rule,test_pw_loader,tests/test_pw_loader.c $(CORE),))
$(eval $(call test_rule,test_pw_segment,tests/test_pw_segment.c src/pw_segment.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_compat32,tests/test_pw_compat32.c src/pw_compat32.c src/pw_segment.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_gate,tests/test_pw_gate.c $(CORE) src/pw_gate.c,))
$(eval $(call test_rule,test_pw_x86_block,tests/test_pw_x86_block.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_cache,tests/test_pw_x86_cache.c src/pw_x86_cache.c,))
$(eval $(call test_rule,test_pw_x86_hostexec,tests/test_pw_x86_hostexec.c src/pw_x86_hostexec.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_engine,tests/test_pw_x86_engine.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_chaining,tests/test_pw_x86_chaining.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_residency,tests/test_pw_x86_residency.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_lazyflags,tests/test_pw_x86_lazyflags.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_guest_call,tests/test_pw_guest_call.c src/pw_guest_call.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_tls,tests/test_pw_tls.c src/pw_tls.c src/pe_tls.c src/pw_guest_call.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c src/pw_map.c src/pe_image.c src/pe_layout.c src/pe_reloc.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_import_bind,tests/test_pw_import_bind.c src/pw_import_bind.c $(CORE),))
$(eval $(call test_rule,test_pw_export,tests/test_pw_export.c src/pw_export.c src/pe_export.c src/pw_import_bind.c $(CORE) src/pw_file_posix.c,))
$(eval $(call test_rule,test_pw_wine_gate,tests/test_pw_wine_gate.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_guest_vm,tests/test_pw_guest_vm.c src/pw_guest_vm.c,))
$(eval $(call test_rule,test_pw_guest_process,tests/test_pw_guest_process.c src/pw_guest_process.c src/pw_vm.c src/pw_vm_posix.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_nt_handle,tests/test_pw_nt_handle.c src/pw_nt_handle.c,))
$(eval $(call test_rule,test_pw_unixlib,tests/test_pw_unixlib.c src/pw_unixlib.c,))
$(eval $(call test_rule,test_pw_wine_runner,tests/test_pw_wine_runner.c src/pw_wine_runner.c,))
$(eval $(call test_rule,test_pw_wine_seed_services,tests/test_pw_wine_seed_services.c src/pw_wine_seed_services.c,))
$(eval $(call test_rule,test_pw_wine_unixlib,tests/test_pw_wine_unixlib.c src/pw_wine_unixlib.c src/pw_unixlib.c src/pw_unix_call.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_wine_handle,tests/test_pw_wine_handle.c src/pw_wine_handle.c src/pw_nt_handle.c,))
$(eval $(call test_rule,test_pw_wine_path,tests/test_pw_wine_path.c src/pw_wine_path.c,))
$(eval $(call test_rule,test_pw_wine_gate_bridge,tests/test_pw_wine_gate_bridge.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_file_service,tests/test_pw_wine_file_service.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_registry,tests/test_pw_wine_registry.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_objects,tests/test_pw_wine_objects.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_process_info,tests/test_pw_wine_process_info.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_virtual_memory,tests/test_pw_wine_virtual_memory.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_section,tests/test_pw_wine_section.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_thread,tests/test_pw_wine_thread.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_continue,tests/test_pw_wine_continue.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_vm_transactions,tests/test_pw_wine_vm_transactions.c $(WINE_GATE) $(CORE),))
$(eval $(call test_rule,test_pw_wine_teardown,tests/test_pw_wine_teardown.c src/pw_guest_process.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c $(CORE),))
$(eval $(call test_rule,test_pw_win32,tests/test_pw_win32.c src/pw_win32.c src/pw_user32.c src/pw_gdi.c src/pw_crt_format.c src/pw_registry.c src/pw_guest_heap.c src/pw_guest_args.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_module_name.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_guest_args,tests/test_pw_guest_args.c src/pw_guest_args.c,))
$(eval $(call test_rule,test_pw_time,tests/test_pw_time.c src/pw_win32.c src/pw_user32.c src/pw_gdi.c src/pw_crt_format.c src/pw_registry.c src/pw_guest_heap.c src/pw_guest_args.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_module_name.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_guest_fp,tests/test_pw_guest_fp.c src/pw_guest_fp.c,))
$(eval $(call test_rule,test_pw_x87,tests/test_pw_x87.c src/pw_x87.c src/pw_guest_fp.c,))
$(eval $(call test_rule,test_pw_audio_ps5,tests/test_pw_audio_ps5.c native/pw_audio_ps5.c,-DPW_AUDIO_PS5_HOST_TEST -pthread))
$(eval $(call test_rule,test_pw_agc_submit_lifecycle,tests/test_pw_agc_submit_lifecycle.c native/pw_agc_submit_lifecycle.c,))
$(eval $(call test_rule,test_pw_videoout_layout,tests/test_pw_videoout_layout.c,))
$(eval $(call test_rule,test_pw_pad_ps5,tests/test_pw_pad_ps5.c native/pw_pad_ps5.c src/pw_pad.c src/pw_user32.c,-DPW_PAD_PS5_HOST_TEST))
$(eval $(call test_rule,test_pw_state_ps5,tests/test_pw_state_ps5.c native/pw_state_ps5.c src/pw_registry_store.c src/pw_registry.c,-DPW_STATE_PS5_HOST_TEST))
$(eval $(call test_rule,test_pw_initterm,tests/test_pw_initterm.c src/pw_win32.c src/pw_user32.c src/pw_gdi.c src/pw_crt_format.c src/pw_registry.c src/pw_guest_heap.c src/pw_guest_args.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_module_name.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_window,tests/test_pw_window.c src/pw_win32.c src/pw_user32.c src/pw_gdi.c src/pw_crt_format.c src/pw_registry.c src/pw_guest_heap.c src/pw_guest_args.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_module_name.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,trace_x86_entry,tools/trace_x86_entry.c src/pe_resource.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_win32.c src/pw_user32.c src/pw_gdi.c src/pw_crt_format.c src/pw_registry.c src/pw_guest_heap.c src/pw_guest_args.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_import_bind.c $(CORE),))
$(eval $(call test_rule,classify_x86,tools/classify_x86.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c,))
$(eval $(call test_rule,dbt_differential,tools/dbt_differential.c src/pw_x86_hostexec.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_win64,tests/test_pw_win64.c src/pw_exec_probe.c src/pw_win64_call.S $(CORE),))
$(eval $(call test_rule,inspect_pe,tools/inspect_pe.c $(CORE) src/pw_file_posix.c,))
$(eval $(call test_rule,bench_dynarec,tools/bench_dynarec.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,-lm))
$(eval $(call test_rule,pw_x86_decode_probe,tools/pw_x86_decode_probe.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,wine_ntdll_entry,tools/wine_ntdll_entry.c $(WINE_GATE) src/pw_file_posix.c src/pw_wine_seed_services.c src/pw_app_profile.c $(CORE),))

BOX86_SOURCE ?= .deps/box86
box86-catalog: $(BUILD)/pw_x86_decode_probe
	@test -f "$(BOX86_SOURCE)/src/emu/x86run.c" || \
		{ echo 'box86-catalog: set BOX86_SOURCE to the pinned Box86 checkout' >&2; exit 2; }
	python3 tools/box86_opcode_catalog.py --box86-source "$(BOX86_SOURCE)" \
		--probe "$(BUILD)/pw_x86_decode_probe" \
		--json-output data/box86_opcode_catalog.json \
		--markdown-output docs/BOX86_OPCODE_CATALOG.md

TESTS := test_pw_x86_hostexec test_pw_guest_heap test_pw_registry test_pw_registry_store test_pw_ini test_pw_app_profile test_pw_profile_catalog test_pw_prefix test_pw_prefix_ps5 test_pw_profile_session_flow test_pw_runtime_supervisor test_launcher_model test_prefix_launcher test_pw_gdi test_pw_gdi_abi test_pw_present test_pw_wine_heap test_pw_wine_prx test_pw_launcher_render test_pw_crt_format test_pw_user32 test_pw_pad test_pe_resource test_pw_time test_pw_guest_args test_pw_initterm test_pw_window test_pw_guest_fp test_pe_image test_pe_layout test_pe_reloc test_pe_import \
	test_pe_export \
	test_pw_module_name test_pw_file_posix test_pw_file_ps5 test_pw_vm test_pw_map test_pw_loader \
	test_pw_segment test_pw_compat32 test_pw_guest_vm test_pw_guest_process test_pw_nt_handle test_pw_unixlib test_pw_wine_runner test_pw_wine_seed_services test_pw_wine_unixlib test_pw_wine_handle test_pw_wine_path test_pw_gate test_pw_win64 test_pw_x86_block test_pw_x86_cache test_pw_x86_engine test_pw_x86_chaining test_pw_x86_residency test_pw_x86_lazyflags test_pw_guest_call test_pw_tls test_pw_import_bind test_pw_export test_pw_wine_gate test_pw_wine_gate_bridge test_pw_wine_file_service test_pw_wine_registry test_pw_wine_objects test_pw_wine_process_info test_pw_wine_virtual_memory test_pw_wine_section test_pw_wine_thread test_pw_wine_continue test_pw_wine_vm_transactions test_pw_wine_teardown test_pw_win32 test_pw_x87 test_pw_audio_ps5 test_pw_agc_submit_lifecycle test_pw_videoout_layout test_pw_pad_ps5 test_pw_state_ps5

# The Python suites drive the built binaries: the evidence validator is
# tested against a transcript the real gate produced, and the Python PE
# encoder is cross-checked against the C parser.
test: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/inspect_pe $(BUILD)/trace_x86_entry $(BUILD)/classify_x86 $(BUILD)/dbt_differential $(BUILD)/bench_dynarec $(BUILD)/wine_ntdll_entry
	@set -e; for test in $(addprefix $(BUILD)/,$(TESTS)); do $$test; done
	python3 tests/test_title_identity.py
	python3 tests/test_runtime_profile_selection.py
	python3 tests/test_icon.py
	python3 tests/test_docs_links.py
	python3 tests/test_native_contract.py
	python3 tests/test_make_test_pe.py
	python3 tests/test_validate_pe_map_evidence.py
	python3 tests/test_validate_runtime_evidence.py
	python3 tests/test_x86_differential.py
	$(BUILD)/dbt_differential < tests/fixtures/dbt_differential_forms.txt
	python3 tests/test_pw_sse_matrix.py
	python3 tests/test_trace_x86_entry.py
	python3 tests/test_inventory_imports.py
	python3 tests/test_wine_runtime_modules.py
	python3 tests/test_wine_runtime_manifest.py
	python3 tests/test_stage_wine_runtime.py
	python3 tests/test_wine_ntdll_evidence.py
	python3 tests/test_unix_call_table.py
	python3 tests/test_unixlib_table.py
	python3 tests/test_wowprospero_contract.py
	python3 tests/test_build_wine_ps5.py
	python3 tests/test_nt_handler_ledger.py
	python3 tests/test_reentrancy_contract.py
	python3 tests/test_test_reachability.py
	python3 tests/test_wine_gate_host.py
	python3 tests/test_audit_wine_imports.py
	python3 tests/test_win32_catalog.py
	python3 tests/test_status_vocabulary.py
	python3 tests/test_support_matrix.py
	python3 tests/test_classify_x86.py
	python3 tests/test_box86_opcode_catalog.py
	python3 tests/test_startup_x87_contract.py
	python3 tests/test_build_source_oracle.py
	python3 tests/test_dynarec_bench.py
	rm -rf tools/__pycache__ tests/__pycache__

# Release evidence. `make test` skips two checks when the pinned Wine checkout
# or the staged runtime are absent, which is right on a machine that has
# neither and wrong when a release's evidence is being produced: this target
# fails instead of skipping, and re-runs the two suites that would have skipped
# so their output is in the transcript.
wine-check: test
	@test -f "$(WINE_SOURCE)/dlls/ntdll/ntsyscalls.h" || \
		{ echo "wine-check: no pinned Wine source at $(WINE_SOURCE)" >&2; exit 2; }
	PROSPERO_WINE_SOURCE="$(WINE_SOURCE)" python3 tests/test_unix_call_table.py
	PROSPERO_WINE_SOURCE="$(WINE_SOURCE)" python3 tests/test_unixlib_table.py
	PROSPERO_WINE_SOURCE="$(WINE_SOURCE)" python3 tests/test_wowprospero_contract.py
	@test -f .deps/wine-runtime/lib/i386-windows/ntdll.dll || \
		{ echo "wine-check: no staged runtime (tools/build_wine_runtime.sh)" >&2; exit 2; }
	python3 tests/test_wine_runtime_manifest.py
	# `make test` cleans the build directory at the end, and the host gate is
	# the runner binary, so it has to exist again before it can run.
	$(MAKE) $(BUILD)/wine_ntdll_entry
	python3 tests/test_wine_gate_host.py

audit:
	python3 tools/audit_publication.py

check-whitespace:
	@# Patches keep diff context byte-for-byte; an empty context line is a space.
	@if git grep -nI -E '[[:blank:]]+$$' -- . ':!*.patch'; then \
		echo 'whitespace check failed: trailing blanks found' >&2; \
		exit 1; \
	else \
		echo 'whitespace check passed'; \
	fi

# Keep sanitizer objects in their own tree: no stale normal binary can satisfy
# the instrumented gate, and subsequent sanitizer runs can reuse its objects.
sanitize:
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) test BUILD=build/sanitize CC=clang CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined'

# Structural report for a private Windows binary. Nothing is copied here.
#   make inspect PE_INPUT=/private/path/game.exe PE_DIR=/private/path
inspect: $(BUILD)/inspect_pe
	@test -n "$(PE_INPUT)" || { echo 'PE_INPUT is required' >&2; exit 2; }
	$(BUILD)/inspect_pe "$(PE_INPUT)" $(if $(PE_DIR),--dir "$(PE_DIR)",)

# Inventory a target even before its DLLs or low-address backend are available.
inspect-only: $(BUILD)/inspect_pe
	@test -n "$(PE_INPUT)" || { echo 'PE_INPUT is required' >&2; exit 2; }
	$(BUILD)/inspect_pe "$(PE_INPUT)" --no-map

# Synthetic images for a gate run that needs no proprietary input at all.
sample:
	@test -n "$(OUT_DIR)" || { echo 'OUT_DIR is required' >&2; exit 2; }
	python3 tools/make_test_pe.py --out-dir "$(OUT_DIR)"

native: test
	$(MAKE) audit check-whitespace
	PW_SAMPLE=1 tools/build_native.sh

native-release: test
	$(MAKE) audit check-whitespace
	tools/build_native.sh

clean:
	rm -rf build dist release tools/__pycache__ tests/__pycache__
