Import("env")

import os


def _existing(paths):
    return [p for p in paths if os.path.isdir(p)]


pio_env = env.subst("$PIOENV")
libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")

framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")

include_candidates = [
    os.path.join(libdeps_dir, pio_env, "AsyncTCP", "src"),
]

if framework_dir:
    include_candidates.extend(
        [
            os.path.join(framework_dir, "libraries", "FS", "src"),
            os.path.join(framework_dir, "libraries", "Network", "src"),
        ]
    )

force_includes = _existing(include_candidates)
if force_includes:
    env.Append(CPPPATH=force_includes)
    print("TempNode: forcing include paths:", ", ".join(force_includes))
else:
    print("TempNode: no fallback include paths found")
