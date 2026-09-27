CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
BUILD := build/host
# Where the pinned Wine checkout lives for the release-evidence target: the
# conventional ignored location, overridable with
# `make wine-check WINE_SOURCE=<checkout>`.
WINE_SOURCE ?= .deps/wine/source
HEADERS := $(wildcard include/*.h src/*.h native/*.h tests/*.h)

.PHONY: all test wine-check sanitize audit check-whitespace native native-release box86-catalog clean

all: test
	$(MAKE) audit check-whitespace

$(BUILD):
	mkdir -p $@

comma := ,

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

$(eval $(call test_rule,test_pw_app_profile,tests/test_pw_app_profile.c src/pw_app_profile.c,))
$(eval $(call test_rule,test_pw_profile_catalog,tests/test_pw_profile_catalog.c src/pw_profile_catalog.c,))
$(eval $(call test_rule,test_pw_present,tests/test_pw_present.c src/pw_present.c,))
$(eval $(call test_rule,test_pw_wine_heap,tests/test_pw_wine_heap.c wine/ps5/pw_wine_heap.c,-pthread))
$(eval $(call test_rule,test_pw_wine_prx,tests/test_pw_wine_prx.c wine/ps5/pw_wine_prx.c,-I.))
$(eval $(call test_rule,test_pw_wine_threads,tests/test_pw_wine_threads.c wine/ps5/pw_wine_threads.c,-pthread))
$(eval $(call test_rule,test_pw_wine_sink,tests/test_pw_wine_sink.c wine/ps5/pw_wine_sink.c,-pthread))
$(eval $(call test_rule,test_pw_wine_start,tests/test_pw_wine_start.c src/pw_wine_start.c wine/ps5/pw_wine_prx.c,-I.))
$(eval $(call test_rule,test_pw_wine_launch,tests/test_pw_wine_launch.c src/pw_wine_launch.c,))
$(eval $(call test_rule,test_pw_game_profile,tests/test_pw_game_profile.c src/pw_game_profile.c src/pw_app_profile.c,))
$(eval $(call test_rule,test_pw_wine_library,tests/test_pw_wine_library.c native/pw_wine_library.c src/pw_game_profile.c src/pw_app_profile.c src/pw_profile_catalog.c,-D_DEFAULT_SOURCE))
$(eval $(call test_rule,test_pw_wine_display,tests/test_pw_wine_display.c native/pw_wine_display.c src/pw_game_profile.c src/pw_app_profile.c,-pthread))
$(eval $(call test_rule,test_pw_wine_dl,tests/test_pw_wine_dl.c wine/ps5/pw_wine_dl.c wine/ps5/pw_wine_prx.c,-pthread))
# The modules and the test are linked with --wrap for every __wrap_ name
# the virtual working directory defines; the test turns fortify off, since
# glibc would redirect realpath and friends to *_chk names --wrap misses.
PW_CWD_WRAPS := $(sort $(patsubst __wrap_%,%,$(filter-out __wrap_,$(shell grep -o '__wrap_[a-z_]*' wine/ps5/pw_wine_cwd_libc.c))))
PW_CWD_TEST_FLAGS := -std=gnu11 -pthread -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	$(addprefix -Wl$(comma)--wrap=,$(PW_CWD_WRAPS))
$(eval $(call test_rule,test_pw_wine_cwd,tests/test_pw_wine_cwd.c wine/ps5/pw_wine_cwd.c wine/ps5/pw_wine_cwd_libc.c,$(PW_CWD_TEST_FLAGS)))
$(eval $(call test_rule,test_pw_wine_compat,tests/test_pw_wine_compat.c wine/ps5/pw_wine_compat.c,-std=gnu11))
$(eval $(call test_rule,test_pw_launcher_render,tests/test_pw_launcher_render.c src/pw_launcher_render.c,))
$(eval $(call test_rule,test_pw_pad,tests/test_pw_pad.c src/pw_pad.c,))
$(eval $(call test_rule,test_pw_vm,tests/test_pw_vm.c src/pw_vm.c src/pw_vm_posix.c src/pw_result.c,))
$(eval $(call test_rule,test_pw_x86_block,tests/test_pw_x86_block.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_flat,tests/test_pw_x86_flat.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_cache,tests/test_pw_x86_cache.c src/pw_x86_cache.c,))
$(eval $(call test_rule,test_pw_x86_code_pages,tests/test_pw_x86_code_pages.c,))
$(eval $(call test_rule,test_pw_x86_hostexec,tests/test_pw_x86_hostexec.c src/pw_x86_hostexec.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_engine,tests/test_pw_x86_engine.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_chaining,tests/test_pw_x86_chaining.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_residency,tests/test_pw_x86_residency.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_x86_global_residency,tests/test_pw_x86_global_residency.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,-D_DEFAULT_SOURCE))
$(eval $(call test_rule,test_pw_x86_reencode,tests/test_pw_x86_reencode.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,-D_DEFAULT_SOURCE))
$(eval $(call test_rule,test_pw_x86_lazyflags,tests/test_pw_x86_lazyflags.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_guest_call,tests/test_pw_guest_call.c src/pw_guest_call.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,test_pw_guest_fp,tests/test_pw_guest_fp.c src/pw_guest_fp.c,))
$(eval $(call test_rule,test_pw_x87,tests/test_pw_x87.c src/pw_x87.c src/pw_guest_fp.c,))
$(eval $(call test_rule,test_pw_audio_ps5,tests/test_pw_audio_ps5.c native/pw_audio_ps5.c,-DPW_AUDIO_PS5_HOST_TEST))
$(eval $(call test_rule,test_pw_audio_mix,tests/test_pw_audio_mix.c src/pw_audio_mix.c,-lm))
$(eval $(call test_rule,test_pw_agc_submit_lifecycle,tests/test_pw_agc_submit_lifecycle.c native/pw_agc_submit_lifecycle.c,))
$(eval $(call test_rule,test_pw_videoout_layout,tests/test_pw_videoout_layout.c,))
$(eval $(call test_rule,test_pw_videoout_tile,tests/test_pw_videoout_tile.c src/pw_present.c,))
$(eval $(call test_rule,test_pw_pad_ps5,tests/test_pw_pad_ps5.c native/pw_pad_ps5.c src/pw_pad.c,-DPW_PAD_PS5_HOST_TEST))
$(eval $(call test_rule,test_pw_data_mount,tests/test_pw_data_mount.c native/pw_data_mount.c,))
PW_DATA_MOUNT_TEST_FLAGS := -DPW_DATA_MOUNT_REQUEST_PATH='"/tmp/pw_dm_req"' -DPW_DATA_MOUNT_PATH='"/tmp/pw_dm_data"' -DPW_DATA_MOUNT_WAIT_MS=200 -DPW_DATA_MOUNT_POLL_MS=50
$(eval $(call test_rule,test_pw_data_mount_native,tests/test_pw_data_mount_native.c native/pw_data_mount.c,$(PW_DATA_MOUNT_TEST_FLAGS)))
$(eval $(call test_rule,classify_x86,tools/classify_x86.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c,))
$(eval $(call test_rule,dbt_differential,tools/dbt_differential.c src/pw_x86_hostexec.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,))
$(eval $(call test_rule,bench_dynarec,tools/bench_dynarec.c src/pw_x86_engine.c src/pw_x86_cache.c src/pw_x86_block.c src/pw_x86_reencode.c src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c,-lm))
$(eval $(call test_rule,pw_x86_decode_probe,tools/pw_x86_decode_probe.c src/pw_x86_block.c src/pw_x87.c src/pw_guest_fp.c src/pw_guest_call.c src/pw_vm.c src/pw_vm_posix.c,))

BOX86_SOURCE ?= .deps/box86
box86-catalog: $(BUILD)/pw_x86_decode_probe
	@test -f "$(BOX86_SOURCE)/src/emu/x86run.c" || \
		{ echo 'box86-catalog: set BOX86_SOURCE to the pinned Box86 checkout' >&2; exit 2; }
	python3 tools/box86_opcode_catalog.py --box86-source "$(BOX86_SOURCE)" \
		--probe "$(BUILD)/pw_x86_decode_probe" \
		--json-output data/box86_opcode_catalog.json \
		--markdown-output docs/BOX86_OPCODE_CATALOG.md

TESTS := test_pw_x86_hostexec test_pw_app_profile test_pw_profile_catalog test_pw_present \
	test_pw_wine_heap test_pw_wine_prx test_pw_wine_start test_pw_wine_launch test_pw_game_profile \
	test_pw_wine_library test_pw_wine_display test_pw_wine_dl test_pw_wine_sink \
	test_pw_wine_threads test_pw_wine_compat test_pw_wine_cwd test_pw_launcher_render test_pw_pad \
	test_pw_guest_fp test_pw_vm test_pw_x86_block test_pw_x86_flat test_pw_x86_cache test_pw_x86_code_pages \
	test_pw_x86_engine test_pw_x86_chaining test_pw_x86_residency test_pw_x86_global_residency test_pw_x86_reencode test_pw_x86_lazyflags \
	test_pw_guest_call test_pw_x87 test_pw_audio_ps5 test_pw_audio_mix test_pw_agc_submit_lifecycle \
	test_pw_videoout_layout test_pw_videoout_tile test_pw_pad_ps5 test_pw_data_mount \
	test_pw_data_mount_native

# The Python suites drive the built DBT tools and check the contracts the
# host compiler cannot.
test: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/classify_x86 $(BUILD)/dbt_differential $(BUILD)/bench_dynarec
	@set -e; for test in $(addprefix $(BUILD)/,$(TESTS)); do $$test; done
	python3 tests/test_title_identity.py
	python3 tests/test_icon.py
	python3 tests/test_docs_links.py
	python3 tests/test_native_contract.py
	python3 tests/test_x86_differential.py
	$(BUILD)/dbt_differential < tests/fixtures/dbt_differential_forms.txt
	$(BUILD)/dbt_differential global < tests/fixtures/dbt_differential_forms.txt
	python3 tests/test_pw_sse_matrix.py
	python3 tests/test_wine_runtime_manifest.py
	python3 tests/test_wowprospero_contract.py
	python3 tests/test_build_wine_ps5.py
	python3 tests/test_gen_prx_descriptor.py
	python3 tests/test_test_reachability.py
	python3 tests/test_status_vocabulary.py
	python3 tests/test_classify_x86.py
	python3 tests/test_box86_opcode_catalog.py
	python3 tests/test_startup_x87_contract.py
	python3 tests/test_build_source_oracle.py
	python3 tests/test_dynarec_bench.py
	python3 tests/test_bench_7zip.py
	rm -rf tools/__pycache__ tests/__pycache__

# Release evidence. `make test` skips the checks that need the pinned Wine
# checkout or the staged host runtime when they are absent, which is right on
# a machine that has neither and wrong when a release's evidence is being
# produced: this target fails instead of skipping.
wine-check: test
	@test -f "$(WINE_SOURCE)/dlls/ntdll/ntsyscalls.h" || \
		{ echo "wine-check: no pinned Wine source at $(WINE_SOURCE)" >&2; exit 2; }
	PROSPERO_WINE_SOURCE="$(WINE_SOURCE)" python3 tests/test_wowprospero_contract.py
	@test -f .deps/wine-runtime/lib/i386-windows/ntdll.dll || \
		{ echo "wine-check: no staged runtime (tools/build_wine_runtime.sh)" >&2; exit 2; }
	python3 tests/test_wine_runtime_manifest.py

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

native: test
	$(MAKE) audit check-whitespace
	PW_SAMPLE=1 tools/build_native.sh

native-release: test
	$(MAKE) audit check-whitespace
	tools/build_native.sh

clean:
	rm -rf build dist release tools/__pycache__ tests/__pycache__
