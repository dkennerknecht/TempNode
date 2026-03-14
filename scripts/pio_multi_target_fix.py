Import("env")

import os
import subprocess
import sys
from SCons.Script import COMMAND_LINE_TARGETS, Exit


def _run(cmd, cwd, env_vars):
    print("TempNode: running", " ".join(cmd))
    return subprocess.call(cmd, cwd=cwd, env=env_vars)


targets = set(COMMAND_LINE_TARGETS or [])
needs_split = "upload" in targets and "uploadfs" in targets

if needs_split and os.environ.get("TEMPNODE_PIO_SPLIT_DONE") != "1":
    project_dir = env.subst("$PROJECT_DIR")
    pio_env = env.subst("$PIOENV")

    child_env = os.environ.copy()
    child_env["TEMPNODE_PIO_SPLIT_DONE"] = "1"

    pio_base = [sys.executable, "-m", "platformio"]
    rc = _run(pio_base + ["run", "-e", pio_env, "-t", "upload"], project_dir, child_env)
    if rc == 0:
        rc = _run(pio_base + ["run", "-e", pio_env, "-t", "uploadfs"], project_dir, child_env)
    if rc == 0 and "monitor" in targets:
        rc = _run(pio_base + ["device", "monitor", "-e", pio_env], project_dir, child_env)

    Exit(rc)
