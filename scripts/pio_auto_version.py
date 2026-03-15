Import("env")

import os
import re
from pathlib import Path


def _safe_int(value, default):
    try:
        return int(str(value).strip())
    except Exception:
        return default


project_dir = Path(env["PROJECT_DIR"])
release_version = str(env.GetProjectOption("custom_release_version", "")).strip()
if not release_version:
    # Backward-compatible fallback for old config style.
    major = _safe_int(env.GetProjectOption("custom_version_major", "1"), 1)
    minor = _safe_int(env.GetProjectOption("custom_version_minor", "0"), 0)
    patch = _safe_int(env.GetProjectOption("custom_version_patch", "0"), 0)
    release_version = f"{major}.{minor}.{patch}"

if not re.fullmatch(r"\d+\.\d+\.\d+", release_version):
    print(
        "TempNode: invalid custom_release_version='{}', fallback to 1.0.0".format(
            release_version
        )
    )
    release_version = "1.0.0"

# Allow explicit override for reproducible CI builds.
forced_version = str(os.environ.get("TEMPNODE_APP_VERSION", "")).strip()

counter_file = project_dir / ".tempnode_local_build"
legacy_counter_file_v1 = project_dir / ".tempnode_build_patch"
legacy_counter_file = project_dir / ".pio" / "generated" / "build_version_patch.txt"

if forced_version:
    version = forced_version
else:
    patch = 0
    if counter_file.exists():
        patch = _safe_int(counter_file.read_text(encoding="utf-8").strip(), 0)
    elif legacy_counter_file_v1.exists():
        patch = _safe_int(legacy_counter_file_v1.read_text(encoding="utf-8").strip(), 0)
    elif legacy_counter_file.exists():
        patch = _safe_int(legacy_counter_file.read_text(encoding="utf-8").strip(), 0)
    patch += 1
    counter_file.write_text(f"{patch}\n", encoding="utf-8")
    # Local builds use a 4th internal segment (X.Y.Z.N), while Git tags stay X.Y.Z.
    version = f"{release_version}.{patch}"

env.Append(CPPDEFINES=[("TEMPNODE_APP_VERSION", '\\"%s\\"' % version)])
print(f"TempNode: release base {release_version}")
print(f"TempNode: build version {version}")
