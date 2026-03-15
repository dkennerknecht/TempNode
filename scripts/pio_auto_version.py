Import("env")

import os
from pathlib import Path


def _safe_int(value, default):
    try:
        return int(str(value).strip())
    except Exception:
        return default


project_dir = Path(env["PROJECT_DIR"])
major = _safe_int(env.GetProjectOption("custom_version_major", "1"), 1)
minor = _safe_int(env.GetProjectOption("custom_version_minor", "0"), 0)

# Allow explicit override for reproducible CI builds.
forced_version = str(os.environ.get("TEMPNODE_APP_VERSION", "")).strip()

counter_file = project_dir / ".tempnode_build_patch"
legacy_counter_file = project_dir / ".pio" / "generated" / "build_version_patch.txt"

if forced_version:
    version = forced_version
else:
    patch = 0
    if counter_file.exists():
        patch = _safe_int(counter_file.read_text(encoding="utf-8").strip(), 0)
    elif legacy_counter_file.exists():
        patch = _safe_int(legacy_counter_file.read_text(encoding="utf-8").strip(), 0)
    patch += 1
    counter_file.write_text(f"{patch}\n", encoding="utf-8")
    version = f"{major}.{minor}.{patch}"

env.Append(CPPDEFINES=[("TEMPNODE_APP_VERSION", '\\"%s\\"' % version)])
print(f"TempNode: build version {version}")
