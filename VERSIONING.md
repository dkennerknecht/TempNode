# Versioning Guide

This project uses two version streams on purpose:

- Build version for OTA safety checks on devices
- Git tag/release version for official GitHub releases

## 1. Local Build Version (Device/OTA)

- Generated automatically on every `pio run` by `scripts/pio_auto_version.py`
- Format: `X.Y.Z.N` where `N` is an internal local build counter
- `X.Y.Z` comes from `custom_release_version` in `platformio.ini`
- Keep `custom_release_version` at your current/next release line (for example `1.2.20`) so local OTA builds remain upgrade-compatible
- Stored counter file: `.tempnode_local_build` (local only, do not commit)
- Injected into `esp_app_desc.version` and exposed by `GET /api/v1/system` as `appVersion`

Why this matters:

- OTA with `ota.allowDowngrade=false` accepts only `incoming > current`
- Re-uploading the same image (same version) is rejected by design

## 2. GitHub Release Version (Official)

- Source of truth: Git tag in format `v<major>.<minor>.<patch>`
- Workflow: `.github/workflows/release-firmware.yml`
- Workflow sets `TEMPNODE_APP_VERSION` from the tag (without leading `v`)
- Release artifacts are built with exactly `X.Y.Z` (no internal 4th segment) and attached to GitHub Releases

## Recommended Team Workflow

1. Develop and test locally with normal builds (`pio run ...`).
2. Verify device version via `GET /api/v1/system` (`appVersion`).
3. Choose a release tag that is higher than deployed device versions.
4. Create and push tag:

```bash
git tag -a v1.2.19 -m "Release v1.2.19"
git push origin v1.2.19
```

5. Roll out OTA using release artifacts from GitHub.

## Common Pitfall

If OTA says:

`incoming version X <= current Y`

then one of these is true:

- You are uploading an image with same or lower version than the device
- Your test device already runs a newer local build

Fix options:

- Build/tag a higher version and retry (recommended)
- Temporarily allow downgrade (`ota.allowDowngrade=true`) only for controlled recovery/testing

## CI/Manual Override

For reproducible builds, set:

`TEMPNODE_APP_VERSION=1.2.500`

Example:

```bash
TEMPNODE_APP_VERSION=1.2.500 pio run -e esp32s3
```

Use this carefully in local dev, because it bypasses automatic internal build counter incrementing.
