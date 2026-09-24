#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static contract for the native runtime's optional app-profile boot path."""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_native_runtime_loads_only_bounded_supported_profiles() -> None:
    source = (ROOT / "native/runtime_main.c").read_text(encoding="utf-8")
    assert '"app.profile"' in source
    assert "PW_APP_PROFILE_MAX_BYTES" in source
    assert "pw_app_profile_parse(buffer,total,profile)" in source
    assert "app_profile.architecture!=PW_APP_ARCH_PE32" in source
    assert "app_profile.graphics!=PW_APP_GRAPHICS_GDI" in source
    assert "pw_app_profile_stage_name(&app_profile,profile_module" in source
    assert "pw_app_profile_build_command_line(&app_profile,commandline" in source
    assert "pw_app_profile_resolve_staged_file(services->app_profile,path" in source
    assert "pw_file_ps5_stream_open_staged(services->files,staged_name" in source
    assert 'open_guest_file(services,filename,"rb",&handle)' in source
    assert ".app_profile=PW_USE_APP_PROFILE?&app_profile:NULL" in source
    assert ".startup_command_id=PW_USE_APP_PROFILE?app_profile.startup_command_id:101u" in source
    assert '.current_directory=PW_USE_APP_PROFILE?app_profile.working_directory:"C:\\\\game"' in source
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
    assert '"$build/validate_profile_stage" "$dist/win/app.profile" "$dist/win"' in source
    assert 'tools/stage_app_files.py" "$stage_input" "$dist/win/app' in source
    assert source.index('"$build/validate_profile_stage"') < source.index(
        'for source in "${sources[@]}"')


def test_recursive_app_staging_is_case_folded_and_confined() -> None:
    with tempfile.TemporaryDirectory(prefix="app-stage-test-") as directory:
        root = Path(directory)
        source = root / "installed"
        destination = root / "stage" / "app"
        (source / "Maps" / "Night").mkdir(parents=True)
        (source / "PINBALL.EXE").write_bytes(b"exe")
        (source / "Maps" / "Night" / "BOARD.DAT").write_bytes(b"board")
        completed = subprocess.run([
            sys.executable, str(ROOT / "tools/stage_app_files.py"),
            str(source), str(destination),
        ], check=False, capture_output=True, text=True)
        assert completed.returncode == 0, completed.stderr
        assert (destination / "pinball.exe").read_bytes() == b"exe"
        assert (destination / "maps" / "night" / "board.dat").read_bytes() == b"board"

        colliding = root / "colliding"
        (colliding / "A").mkdir(parents=True)
        (colliding / "a").mkdir()
        (colliding / "A" / "same.dat").write_bytes(b"one")
        (colliding / "a" / "SAME.DAT").write_bytes(b"two")
        collision_output = root / "collision-output"
        rejected = subprocess.run([
            sys.executable, str(ROOT / "tools/stage_app_files.py"),
            str(colliding), str(collision_output),
        ], check=False, capture_output=True, text=True)
        assert rejected.returncode != 0
        assert "case-folded path collision" in rejected.stderr
        assert not collision_output.exists()

        linked = root / "linked"
        linked.mkdir()
        (linked / "outside.dat").symlink_to(source / "PINBALL.EXE")
        rejected = subprocess.run([
            sys.executable, str(ROOT / "tools/stage_app_files.py"),
            str(linked), str(root / "symlink-output"),
        ], check=False, capture_output=True, text=True)
        assert rejected.returncode != 0
        assert "symlink" in rejected.stderr
        assert not (root / "symlink-output").exists()


def test_profile_stage_validator_uses_real_manifests() -> None:
    with tempfile.TemporaryDirectory(prefix="profile-stage-test-") as directory:
        binary = Path(directory) / "validate-profile-stage"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "tools/validate_profile_stage.c"),
            str(ROOT / "src/pw_app_profile.c"), "-o", str(binary),
        ], check=True, cwd=ROOT)
        profiles = (
            ("pinball.profile", "pinball.exe", "space-cadet-pinball"),
            ("paint.profile", "mspaint.exe", "paint"),
        )
        for manifest, executable, profile_id in profiles:
            stage = Path(directory) / manifest.removesuffix(".profile")
            stage.mkdir()
            (stage / executable).write_bytes(b"staged PE fixture")
            completed = subprocess.run([
                str(binary), str(ROOT / "examples/profiles" / manifest),
                str(stage),
            ], check=False, capture_output=True, text=True)
            assert completed.returncode == 0, completed.stderr
            assert f"id={profile_id}" in completed.stdout
            (stage / executable).unlink()
            completed = subprocess.run([
                str(binary), str(ROOT / "examples/profiles" / manifest),
                str(stage),
            ], check=False, capture_output=True, text=True)
            assert completed.returncode != 0
            assert executable in completed.stderr


if __name__ == "__main__":
    test_native_runtime_loads_only_bounded_supported_profiles()
    test_builder_stages_profile_explicitly()
    test_recursive_app_staging_is_case_folded_and_confined()
    test_profile_stage_validator_uses_real_manifests()
    print("runtime profile selection contract passed")
