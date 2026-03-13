Import("env")

from pathlib import Path


def patch_onewire():
    project_dir = Path(env["PROJECT_DIR"])
    pioenv = env["PIOENV"]
    one_wire_cpp = project_dir / ".pio" / "libdeps" / pioenv / "OneWire" / "OneWire.cpp"

    if not one_wire_cpp.exists():
        return

    content = one_wire_cpp.read_text(encoding="utf-8")
    old = (
        "#  undef noInterrupts() {portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;portENTER_CRITICAL(&mux)\n"
        "#  undef interrupts() portEXIT_CRITICAL(&mux);}"
    )
    new = "#  undef noInterrupts\n#  undef interrupts"

    if old not in content:
        return

    one_wire_cpp.write_text(content.replace(old, new), encoding="utf-8")
    print("Applied OneWire warning patch")


patch_onewire()
