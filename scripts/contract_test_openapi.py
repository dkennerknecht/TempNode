#!/usr/bin/env python3
"""Small REST contract test: OpenAPI spec vs RestServer route contract."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

HTTP_METHODS = {"get", "post", "put", "delete", "patch", "options", "head"}


def fail(errors: list[str], msg: str) -> None:
    errors.append(msg)


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # pragma: no cover - hard fail path
        raise RuntimeError(f"failed to parse JSON {path}: {exc}") from exc


def extract_routes(rest_cpp: str) -> dict[str, set[str]]:
    routes: dict[str, set[str]] = {}
    pattern = re.compile(r'_srv->on\("([^"]+)",\s*HTTP_([A-Z]+)')
    for path, method in pattern.findall(rest_cpp):
        if not path.startswith("/api/v1/"):
            continue
        routes.setdefault(path, set()).add(method.lower())
    return routes


def resolve_ref(spec: dict[str, Any], ref: str) -> Any:
    if not ref.startswith("#/"):
        raise ValueError(f"unsupported ref: {ref}")
    node: Any = spec
    for part in ref[2:].split("/"):
        if not isinstance(node, dict) or part not in node:
            raise KeyError(f"ref not found: {ref}")
        node = node[part]
    return node


def op_schema(spec: dict[str, Any], op: dict[str, Any], status: str) -> dict[str, Any]:
    responses = op.get("responses", {})
    response = responses.get(status)
    if not isinstance(response, dict):
        return {}
    schema = response.get("content", {}).get("application/json", {}).get("schema", {})
    if isinstance(schema, dict) and "$ref" in schema:
        resolved = resolve_ref(spec, schema["$ref"])
        return resolved if isinstance(resolved, dict) else {}
    return schema if isinstance(schema, dict) else {}


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    spec_path = root / "docs" / "openapi.json"
    rest_path = root / "src" / "RestServer.cpp"

    errors: list[str] = []

    if not spec_path.exists():
        fail(errors, f"missing spec: {spec_path}")
        print("\n".join(f"[FAIL] {e}" for e in errors))
        return 1
    if not rest_path.exists():
        fail(errors, f"missing server file: {rest_path}")
        print("\n".join(f"[FAIL] {e}" for e in errors))
        return 1

    spec = load_json(spec_path)
    rest_cpp = rest_path.read_text(encoding="utf-8")

    routes_in_code = extract_routes(rest_cpp)
    if not routes_in_code:
        fail(errors, "no /api/v1 routes found in RestServer.cpp")

    paths = spec.get("paths", {})
    if not isinstance(paths, dict) or not paths:
        fail(errors, "openapi paths section is missing or empty")
        print("\n".join(f"[FAIL] {e}" for e in errors))
        return 1

    routes_in_spec: dict[str, set[str]] = {}
    for path, item in paths.items():
        if not isinstance(item, dict):
            continue
        methods = {m for m in item.keys() if m in HTTP_METHODS}
        if methods:
            routes_in_spec[path] = methods

    code_paths = set(routes_in_code.keys())
    spec_paths = set(routes_in_spec.keys())

    for p in sorted(code_paths - spec_paths):
        fail(errors, f"missing path in spec: {p}")
    for p in sorted(spec_paths - code_paths):
        fail(errors, f"path present in spec but not code: {p}")

    for path in sorted(code_paths & spec_paths):
        missing_methods = routes_in_code[path] - routes_in_spec[path]
        extra_methods = routes_in_spec[path] - routes_in_code[path]
        for m in sorted(missing_methods):
            fail(errors, f"missing method in spec: {path} {m.upper()}")
        for m in sorted(extra_methods):
            fail(errors, f"method present in spec but not code: {path} {m.upper()}")

    # Basic OpenAPI quality gates for each operation
    for path, methods in routes_in_spec.items():
        item = paths[path]
        for method in methods:
            op = item.get(method, {})
            if not isinstance(op, dict):
                fail(errors, f"operation is not an object: {path} {method.upper()}")
                continue
            if not op.get("operationId"):
                fail(errors, f"missing operationId: {path} {method.upper()}")
            if not isinstance(op.get("responses"), dict) or not op["responses"]:
                fail(errors, f"missing responses: {path} {method.upper()}")

    # Targeted check: health schema must include detailed checks
    health_get = paths.get("/api/v1/health", {}).get("get", {})
    if not isinstance(health_get, dict):
        fail(errors, "missing /api/v1/health GET operation")
    else:
        health_schema = op_schema(spec, health_get, "200")
        checks = health_schema.get("properties", {}).get("checks", {})
        if isinstance(checks, dict) and "$ref" in checks:
            checks = resolve_ref(spec, checks["$ref"])
        checks_props = checks.get("properties", {}) if isinstance(checks, dict) else {}
        for key in ("network", "mqtt", "sd", "time", "ota_state"):
            if key not in checks_props:
                fail(errors, f"health schema missing checks.{key}")

    # Targeted check: security schemes exist
    sec = spec.get("components", {}).get("securitySchemes", {})
    if "bearerAuth" not in sec:
        fail(errors, "missing components.securitySchemes.bearerAuth")
    if "basicAuth" not in sec:
        fail(errors, "missing components.securitySchemes.basicAuth")

    if errors:
        print("Contract test FAILED")
        for e in errors:
            print(f"[FAIL] {e}")
        return 1

    print("Contract test PASSED")
    print(f"Routes checked: {len(routes_in_code)}")
    for path in sorted(routes_in_code):
        methods = ",".join(sorted(m.upper() for m in routes_in_code[path]))
        print(f"  - {path}: {methods}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
