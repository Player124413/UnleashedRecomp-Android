# CI: building the APK with GitHub Actions

The workflow at `.github/workflows/android.yml` builds the APK from source on an
`ubuntu-24.04` runner. It runs on every `v*` tag push and can be started manually
from the Actions tab (workflow_dispatch).

## Required configuration

The build needs three game-derived files that must never be committed to this
public repository: `default.xex`, `default.xexp` and `shader.ar`. Keep them in a
**private** repository and give the workflow access to it:

1. Create a private repository (e.g. `SansNope/UnleashedRecomp-GameFiles`)
   containing `default.xex`, `default.xexp` and `shader.ar` at its root.
2. In this repository's **Settings → Secrets and variables → Actions**:
   - add a **variable** `GAME_FILES_REPO` with the `owner/name` of the private
     repository;
   - add a **secret** `GAME_FILES_TOKEN` with a fine-grained personal access
     token that has read-only *Contents* permission for that single repository.

Note that a private repository limits distribution but does not change the legal
status of game-derived data; keep access to it minimal.

## Optional release signing

Without signing secrets the workflow produces a **debug** APK (signed with the
runner's debug key — installable, but a different signature every run). To get a
release APK signed with a stable key, add these secrets:

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | the keystore file, base64-encoded (`base64 -w0 keystore.jks`) |
| `ANDROID_KEYSTORE_PASSWORD` | keystore password |
| `ANDROID_KEY_ALIAS` | key alias |
| `ANDROID_KEY_PASSWORD` | key password |

Remember that installs only update in place when the signing key stays the same.

## What the workflow does

1. Checks out this repository plus the private game-files repository, staging
   the latter into `UnleashedRecompLib/private/`.
2. Builds the host code-generation tools (XenonRecomp, XenosRecomp, file_to_c,
   x_decompress) natively for the runner — a `UNLEASHED_RECOMP_HOST_TOOLS_ONLY`
   CMake pass, mirroring `build_host_tools.bat`.
3. Clones vcpkg at the `builtin-baseline` from `vcpkg.json` and cross-compiles
   `libmain.so` for `arm64-v8a` (NDK r29, `android-29`), passing the host tools
   via `UNLEASHED_RECOMP_HOST_*` — mirroring `build_android_configure.bat`.
   XenonRecomp regenerates the `ppc/` sources and XenosRecomp recompiles the
   shader archive during this pass.
4. Builds the libadrenotools hook libraries (`libmain_hook.so`,
   `libfile_redirect_hook.so`, `libgsl_alloc_hook.so`, `libhook_impl.so`) from
   the vendored sources and stages all native libraries into
   `android-apk/app/src/main/jniLibs/arm64-v8a/`.
5. Runs Gradle (`assembleRelease` when signing secrets are present, otherwise
   `assembleDebug`) and uploads the APK as a workflow artifact. On tag builds it
   also attaches the APK to the matching GitHub release when one exists.

Both CMake passes use ccache and the vcpkg binary cache; the first run is slow
(the ~265 generated `ppc_recomp.*.cpp` translation units dominate), repeat runs
are much faster.

## Secrets and fork PRs

GitHub does not expose secrets to workflows triggered by pull requests from
forks, so the workflow deliberately runs only on tags and manual dispatch —
never on PRs. Do not weaken that: it is what keeps `GAME_FILES_TOKEN` safe.
