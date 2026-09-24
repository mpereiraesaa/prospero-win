#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static contract for the native runtime's optional app-profile boot path."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_native_runtime_loads_only_bounded_supported_profiles() -> None:
    source = (ROOT / "native/runtime_main.c").read_text(encoding="utf-8")
    assert '"app.profile"' in source
    assert "PW_APP_PROFILE_MAX_BYTES" in source
    assert "pw_app_profile_parse(buffer,total,profile)" in source
    assert "app_profile.architecture!=PW_APP_ARCH_PE32" in source
    assert "app_profile.graphics!=PW_APP_GRAPHICS_GDI" in source
    assert "app_profile_stage_name(app_profile.executable,profile_module" in source
    assert "value>='A' && value<='Z'?value+('a'-'A'):value" in source
    assert "app_profile_commandline(&app_profile" in source
    assert ".main_module_filename=PW_USE_APP_PROFILE?app_profile.executable" in source
    assert "pw_prefix_ps5_io(&prefix_io)" in source
    assert "PW_PREFIX_PS5_DEFAULT_ROOT" in source
    assert "pw_prefix_open(&prefix_service,app_profile.prefix" in source
    assert "prefix_registry_path(prefix_layout,profile_registry_path" in source
    assert "registry_path=profile_registry_path" in source
    assert "pw_state_ps5_load_registry(&registry,registry_path" in source
    assert "pw_state_ps5_save_registry_ex(&registry,registry_path" in source
    assert "pw_runtime_supervisor_begin(supervisor,&app_profile" in source
    assert "pw_runtime_supervisor_guest_started(supervisor)" in source
    assert "pw_runtime_supervisor_request_stop(supervisor)" in source
    assert "pw_runtime_supervisor_guest_exited(supervisor" in source
    assert "pw_runtime_supervisor_cleanup_complete(" in source
    assert "PW_RUNTIME_TEARDOWN schema=2" in source


def test_builder_stages_profile_explicitly() -> None:
    source = (ROOT / "tools/build_native.sh").read_text(encoding="utf-8")
    assert "app_profile=${PW_APP_PROFILE:-}" in source
    assert "-DPW_USE_APP_PROFILE=\"$use_app_profile\"" in source
    assert '"$dist/win/app.profile"' in source
    assert "profile_bytes <= 8192" in source


if __name__ == "__main__":
    test_native_runtime_loads_only_bounded_supported_profiles()
    test_builder_stages_profile_explicitly()
    print("runtime profile selection contract passed")
