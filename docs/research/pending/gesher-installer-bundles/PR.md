Title: Install app bundles as Android would: content detection, bundletool APK sets, XAPK manifests

Base: `main` at 2dd7d10d (after #385, "Support APKM and APK+ split packages in Windows").
Branch: `installer-bundles`, six commits; patches 0001-0006 in this folder.

## Change and purpose

the other runtime installs an app from a single APK or from a ZIP of APKs (a bundletool APK
set `.apks`, APKPure's `.xapk`, APKMirror's `.apkm`, `.apk+`). #385 already
added `.apkm`/`.apk+` to the association, the Control Center filters and the icon
handler, picked the base APK by its manifest instead of by file name, and
guarded extraction against path traversal. What was still wrong at 54be6270:

| # | Where (54be6270) | Problem |
|---|---|---|
| 1 | `app/java/com/other/app/PackageBundles.java:13-16`; used at `Installer.java:413` and `IconApks.java:31` | Bundle or APK is decided by suffix only. A single APK saved as `.xapk` is unpacked, has no `.apk` entry and fails ("No base APK", `Installer.java:770`); a ZIP of APKs saved as `.apk` goes to the APK parser and fails. |
| 2 | `Installer.java:740-768` (`extractBundle`), `IconApks.java:36-66` | Every `.apk` entry is taken. A bundletool APK set is not one install: standalone APKs per ABI, a split variant per SDK range, instant APKs and on-demand modules all land in the cluster, so a set fails with "Multiple base APKs" (`Installer.java:765`) / "ambiguous base APK" (`IconApks.java:62`). `docs/installer.md:286-288` told users to "export a single matching set first". `toc.pb` is ignored. |
| 3 | `Installer.java:747-757` | Entries are flattened to their leaf name: `splits/base-master.apk` and a second variant's or another folder's file of the same name collide (`CREATE_NEW` fails). |
| 4 | `Installer.java:348`, `:513-514` | Installed split files are named `split_` + the archive's file name (`split_base-xxhdpi.apk`, `split_display.apk`), not `split_<split name>.apk` as `PackageInstallerSession` (and the other runtime's own session path, `InstalledSplitSet.apkName`) names them. bundletool's split file names therefore never match the session naming. |
| 5 | `Installer.java:324-328`, `:419` | A loose APK takes every `split_*.apk` / `config.*.apk` in its folder as companions, including other apps' splits left in Downloads, and the install then fails on their signature or the cluster check. |
| 6 | `Installer.java:745` | An XAPK's `manifest.json` is ignored: its `split_apks` list (the install) and `expansions` (OBB files) are not read; `.obb` entries are dropped without a word. |
| 7 | `Installer.java:413-419` | An encrypted APKMirror `.apkm` (not a ZIP) reaches the APK parser and fails with a parse error that does not say why. |

Not a problem any more at 54be6270 (fixed by #385, so not touched here): the base
chosen in `listFiles` order (52513f0 `Installer.java:418-425`; now by manifest,
`ApkPackage.isBaseApk`), `.apkm` missing from `other.iss` (now `:207-210`, `:661`),
the panel's dialog and drop filters (`other-control-panel.ps1:2147`, `:2161`) and
`IconApks`. `other_shellext.cc` does not filter by extension (it is registered per
ProgID), `Launcher.java:510-513` and `other-launch` pass the path through.

After this change:

- A file is a bundle when it is a ZIP without `AndroidManifest.xml` at its root that
  holds `.apk` entries; the suffix is not consulted.
- From a bundletool APK set, the other runtime installs what `bundletool install-apks` would
  install on its device, read from `toc.pb`.
- An XAPK with `manifest.json` `split_apks` installs those files.
- Installed files are `base.apk` and `split_<split name>.apk`, from their manifests.
- A loose APK's neighbours of another package are left out (and logged).
- Expansion files are logged as not installed; an encrypted `.apkm` is refused with
  a message that says so.

### Commits

1. `fix(installer): tell an APK from a bundle by its content, not its suffix`
   `PackageBundles.isBundle` opens the file: `AndroidManifest.xml` at the root is an
   APK; otherwise `.apk` entries make a bundle; not a ZIP is not a bundle (the parser
   reports it). New host test `tests/package/PackageBundlesTest.java`, run by
   `tools/test_package_bundles.py` in a small workflow
   (`.github/workflows/package-bundle-contracts.yml`, the same shape as
   `app-data-storage-contracts.yml`). `SplitPackageContractTest` installs a base APK
   saved as `.xapk`; `IconBundleTest` reads an APK named `.xapk` and a bundle named
   `.apk` (its non-ZIP `bad.apks` case is now an APK input, not a rejected bundle).
2. `fix(installer): name a bundle's files by their manifests, not the archive`
   Each selected entry is staged under a generated name, its manifest read
   (`ApkPackage.splitName`, new) and renamed `base.apk` / `split_<split name>.apk`
   (`InstalledSplitSet.apkName`), with the `.dm` that sat beside it. No archive path
   reaches the filesystem; a second APK for the same split is reported. Loose
   companions and `sameSplits` use the same names. `SplitPackageContractTest`'s
   expected split paths become `split_feature.core.apk` / `split_feature.entry.apk`,
   and an `.apk+` holding the installed cluster under other names is now `unchanged`.
3. `fix(installer): leave other apps' splits beside a loose APK out of its install`
   Companions whose manifest names another package are skipped and logged; same-package
   companions are checked as before (`InstallerSigningTest`'s foreign-signer split is
   still refused). `SplitPackageContractTest` puts another package's split beside an
   installed base.
4. `feat(installer): install from a bundletool APK set what the device gets`
   `com.other.app.ApkSetToc` decodes `toc.pb` (protobuf wire format, field numbers
   of bundletool's `commands.proto` / `targeting.proto`) and follows bundletool's
   `ApkMatcher`, `VariantMatcher`, `SdkVersionMatcher`, `AbiMatcher`,
   `MultiAbiMatcher`, `ScreenDensityMatcher` (+ `ScreenDensitySelector`, i.e.
   `ResTable_config::isBetterThan`), `LanguageMatcher`,
   `TextureCompressionFormatMatcher`, `DeviceGroup*Matcher`, `CountrySetApkMatcher`,
   `DeviceTierApkMatcher`, `ModuleMatcher` and `ModuleDependenciesUtils`
   (github.com/google/bundletool, `com.android.tools.build.bundletool.device`):
   one non-instant, non-SDK-runtime variant matching SDK and ABIs (more than one or
   none is an error, as `getMatchingVariant`); install-time modules (delivery type,
   or the deprecated on-demand flag before bundletool 0.10.2) plus dependencies;
   their matching APKs; install-time asset slices. Incompatible sets fail with
   `INSTALL_FAILED_NO_MATCHING_ABIS` / `INSTALL_FAILED_OLDER_SDK`; device-tier
   targeting is refused (bundletool refuses it without a tier in the spec). the other runtime's
   device is `PackageBundles.Device.other(SDK_INT)`: `NativeLibs.SUPPORTED`
   (x86_64, then arm64-v8a) and every density and language split (see ADR 0091).
   The installer and the icon handler use the same selection. `PackageBundlesTest`
   builds a set laid out like `build-apks` output and checks the other runtime's device, a full
   spec (arm64-v8a, 420 dpi, Hebrew), SDK 28 and SDK 19 devices, eight densities and
   five refusals. `SplitPackageContractTest` installs a set whose standalone variant
   is another base APK and expects the installed split cluster, `unchanged`.
5. `feat(installer): follow an XAPK's manifest.json and report what is not installed`
   `split_apks` decides an XAPK's APKs (each must be in the archive); expansion
   files (`expansions`, `.obb` entries) are logged as not installed; a `.apkm` that
   is not a ZIP is refused with `INSTALL_FAILED_INVALID_APK: ... an encrypted
   APKMirror bundle, which the other runtime cannot open`. Tests: v2 and v1 XAPK manifests, a
   manifest naming a missing APK, a malformed manifest, a ZIP with entries marked
   encrypted.
6. `docs: record which APKs of a bundle an install takes (ADR 0091)`
   `docs/decisions/0091-apk-bundle-selection.md`; `app/README.md` points to it.
   (`docs/installer.md` is updated in the commits that change the behaviour.)

## Android contract and project scope

- **Contract.** Android installs a set of APKs that is already one install
  (`PackageInstaller` sessions, `adb install-multiple`); the store or tool that
  holds the bundle chooses that set: Play from the app bundle, `bundletool
  install-apks` from an APK set's `toc.pb`
  (https://developer.android.com/tools/bundletool#deploy_with_bundletool), an XAPK
  installer from `manifest.json`. Split file naming follows
  `PackageInstallerSession` (`split_<splitName>.apk`), which the other runtime's session path
  already uses (`InstalledSplitSet.apkName`). Expansion files live in shared storage
  under `Android/obb/<package>/`
  (https://developer.android.com/google/play/expansion-files); a package install
  does not place them.
- **Application-visible changes.** Bundles that failed now install with the APKs
  the device would get. `ApplicationInfo.splitSourceDirs` of a bundle install now
  names `split_<split name>.apk` as on Android; an existing bundle install gets those
  names on its next install (counted as an update). No permission, capability or
  workflow of the app changes.
- **Windows mechanisms.** None new; this is the installer's choice of input files.
- **Synthetic inputs.** None in production. `Device.other` describes the other runtime's real
  ABIs and SDK level; dimensions the other runtime does not have (texture formats, device
  groups, country set) are described as absent, so only untargeted or fallback APKs
  match, and device tiers are refused rather than guessed. Keeping every density and
  language split installs real APKs from the user's file; it does not claim a
  capability. Test fixtures (synthetic ZIPs and `toc.pb`) are isolated in
  `tests/package` and the dex-services tests.
- **Nothing fabricated or bypassed.** OBB files are reported, not placed or faked;
  an encrypted `.apkm` is refused, not decrypted; signature, cluster and ABI checks
  run on the selected APKs exactly as before.

Within project scope.

## Validation

Run here (Linux, OpenJDK 21/25):

- `python3 tools/test_package_bundles.py`: `PackageBundlesTest all passed: 37 checks`,
  at every commit of the series (8, 8, 8, 28, 37, 37 checks), and after `git am` of
  the exported patches onto 54be6270. Production classes `PackageBundles`,
  `ApkSetToc`, `NativeLibs` on real ZIP files; the `toc.pb` is hand-encoded in the
  test, not produced by bundletool.
- `python3 .github/scripts/test_automerge_workflows.py`: 11 tests OK (workflow added).
- `javac` of `Installer.java`, `IconApks.java`, `ApkPackage.java` and the two
  dex-services tests against `app/java` without the Android framework: the only
  errors are unresolved `android.*` / framework symbols; every the other runtime symbol the
  change uses resolves. This is a partial compile check, not a build.

Not run (need the Windows build, the framework and ART):

- `SplitPackageContractTest`, `IconBundleTest`, `InstallerSigningTest`
  (`tests/windows/tracks/94-framework-fixes.ps1`, `run-shell-icons.ps1`); the new
  and changed cases in them are unverified.
- An install of a real `bundletool build-apks` output, and a comparison of
  `ApkSetToc` with `bundletool extract-apks --device-spec` on the same set: not
  done; the selection follows bundletool's source but is tested on hand-built
  `toc.pb` files only.
- Real XAPK, APKM and encrypted APKM files from the stores.

## Known limits

- Density and language splits are all installed (ADR 0091). The matcher supports
  choosing them; `Device.other` does not use it.
- Conditional modules with device-feature or OpenGL conditions are installed (as
  bundletool does for a device spec that lists no features); device-group
  conditional modules are not.
- Device-tier targeting is refused.
- OBB files are not placed in `Android/obb/<package>/`; an app that needs them will
  not find them. Placing them belongs to the other runtime's shared-storage ownership and is
  left to a separate change. For reference: APKPure's XAPK installer copies them
  there; `adb install` / `install-multiple` never handle them; Google Play downloads
  them there. (SAI's and the APKMirror installer's OBB handling was not checked.)
- The log lines (ignored companions, expansion files) go to the system server log,
  not to the `other-app` console.
- Encrypted `.apkm` is recognized by "named `.apkm` and not readable as a ZIP".

## Contributor checklist

- [x] I have read the contribution policy and checked this change against it.
- [x] I have explained application-visible behavior and identified any unresolved scope question.
- [ ] I have reviewed the submitted code, including any AI-generated changes. (For the submitting human: the code was written by an AI assistant.)
- [x] I have identified third-party code and licenses, if any, and have the right to submit this contribution. (No code copied; bundletool, Apache 2.0, is followed and cited by class name.)

## Reviewer instructions

Read CONTRIBUTING.md and inspect the actual current-head diff. Check project
scope before correctness. A checked box or green CI is not proof of compliance.
Report workflow substitutions, bypasses or fabricated capabilities as
merge blockers, identifying the requested operation, actual behavior, applicable
contract and observable consequence. Preserve useful Windows adaptations; a
different internal implementation is not itself a defect. Requests for removal
or redesign need a concrete contract, correctness, security, performance or
resource finding. Withhold an approval recommendation when scope evidence or a
required Android-contract reference is missing. State the reviewed commit, scope
conclusion, supporting evidence, blockers and validation limits in your review.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_0185vQkPNo1ENZ3Mi9zALKd2
