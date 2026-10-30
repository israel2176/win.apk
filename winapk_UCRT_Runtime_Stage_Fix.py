from pathlib import Path
import sys

MARKER = Path("windows/scripts/stage_runtime.sh")


def find_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        if (candidate / MARKER).is_file():
            return candidate
    for marker in start.rglob(str(MARKER)):
        return marker.parents[2]
    raise FileNotFoundError("win.apk-main with windows/scripts/stage_runtime.sh was not found")


def main() -> int:
    start = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd().resolve()
    root = find_root(start)
    stage_sh = root / "windows/scripts/stage-ucrt-runtime.sh"
    ps1 = root / "run-ucrt-diagnostic.ps1"

    stage = r'''#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-windows-clang"
STAGE="$ROOT/Win.apk-UCRT"

[[ -x "$BUILD/winapk-runtime.exe" ]] || {
  echo "[ERROR] Missing $BUILD/winapk-runtime.exe" >&2
  echo "Build first: ./windows/scripts/build-windows-clang.sh" >&2
  exit 1
}

rm -rf "$STAGE"
"$ROOT/windows/scripts/stage_runtime.sh" "$BUILD" "$STAGE"

[[ -f "$STAGE/lib/android-framework.jar" ]] || {
  echo "[ERROR] stage_runtime.sh did not create $STAGE/lib/android-framework.jar" >&2
  find "$STAGE" -maxdepth 3 -type f -print >&2 || true
  exit 1
}
[[ -x "$STAGE/bin/winapk-runtime.exe" ]] || {
  echo "[ERROR] staged runtime executable is missing" >&2
  exit 1
}
echo "[OK] Staged runtime: $(cygpath -w "$STAGE")"
'''
    stage_sh.write_text(stage, encoding="utf-8", newline="\n")
    stage_sh.chmod(0o755)

    if not ps1.is_file():
        raise FileNotFoundError("run-ucrt-diagnostic.ps1 was not found; run the prior diagnostic generator first")
    text = ps1.read_text(encoding="utf-8-sig")
    backup = ps1.with_suffix(".ps1.stage-fix-backup")
    if not backup.exists():
        backup.write_text(text, encoding="utf-8-sig", newline="\r\n")

    text = text.replace(
        '$Exe = Join-Path $Root "build-windows-clang\\winapk-runtime.exe"',
        '$Stage = Join-Path $Root "Win.apk-UCRT"\n$Exe = Join-Path $Stage "bin\\winapk-runtime.exe"\n$Lib = Join-Path $Stage "lib"'
    )
    text = text.replace(
        'Write-Log "Runtime: $Exe"',
        'Write-Log "Runtime: $Exe"\nWrite-Log "Library: $Lib"'
    )
    text = text.replace(
        'if (-not (Test-Path $Exe)) {\n    Write-Log "ERROR: Runtime executable was not found: $Exe"\n    Write-Log "Build first with: ./windows/scripts/build-windows-clang.sh"\n    exit 2\n}',
        'if (-not (Test-Path $Exe)) {\n    Write-Log "ERROR: Staged runtime was not found: $Exe"\n    Write-Log "Run: ./windows/scripts/stage-ucrt-runtime.sh"\n    exit 2\n}\n$Framework = Join-Path $Lib "android-framework.jar"\nif (-not (Test-Path $Framework)) {\n    Write-Log "ERROR: Android framework was not staged: $Framework"\n    Write-Log "Run: ./windows/scripts/stage-ucrt-runtime.sh"\n    exit 6\n}'
    )
    text = text.replace(
        'Write-Log "Command: winapk-runtime.exe --apk <selected APK> --data <test data>"',
        'Write-Log "Command: winapk-runtime.exe --lib <staged lib> --apk <selected APK> --data <test data>"'
    )
    text = text.replace(
        '& $Exe --apk $Apk --data $DataDir *>&1 |',
        '& $Exe --lib $Lib --apk $Apk --data $DataDir *>&1 |'
    )

    required = [
        'Win.apk-UCRT',
        'android-framework.jar',
        '--lib $Lib --apk $Apk',
        'stage-ucrt-runtime.sh',
    ]
    missing = [x for x in required if x not in text]
    if missing:
        raise RuntimeError("Diagnostic patch verification failed: " + ", ".join(missing))
    ps1.write_text(text, encoding="utf-8-sig", newline="\r\n")

    print(f"[OK] Created: {stage_sh}")
    print(f"[OK] Patched: {ps1}")
    print("[OK] Run: ./windows/scripts/stage-ucrt-runtime.sh")
    print("[OK] Then run: run-ucrt-diagnostic.cmd")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
