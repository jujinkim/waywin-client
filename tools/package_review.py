#!/usr/bin/env python3
"""Build a local review bundle from already-built binaries (developer packaging only)."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "out/release/waywin-0.3.0"


def main():
    companion = ROOT / "build/windows-complete/windows/companion/waywin-companion.exe"
    probe = ROOT / "build/windows-complete/windows/probe/waywin-windows-probe.exe"
    if not companion.is_file() or not probe.is_file():
        raise SystemExit("Build the Windows targets in build/windows-complete first.")
    if DEST.exists():
        shutil.rmtree(DEST)  # Only this tool's fixed ignored output directory.
    (DEST / "windows").mkdir(parents=True)
    source = DEST / "ubuntu/source"
    source.mkdir(parents=True)
    files = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
    for name in sorted(set(filter(None, files))):
        original = ROOT / name
        if not original.is_file():
            continue
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)
    shutil.copy2(companion, DEST / "windows/WayWin Companion.exe")
    shutil.copy2(probe, DEST / "windows/WayWin Windows Probe.exe")
    for name in ("install-linux.sh", "WayWin-Setup.desktop"):
        target = DEST / "ubuntu" / name
        shutil.copy2(ROOT / "packaging" / name, target)
        target.chmod(0o755)
    shutil.copy2(ROOT / "docs/product.md", DEST / "PRODUCT-GUIDE.md")
    shutil.copy2(ROOT / "LICENSE", DEST / "LICENSE")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT))
    (DEST / "SOURCE-REVISION").write_text(revision + (" (working tree changes)" if dirty else "") + "\n")
    (DEST / "START-HERE.md").write_text("""# WayWin 0.3 · 직접 확인하기

1. 이 묶음을 Ubuntu 26.04 GNOME Wayland와 Windows PC에 복사하세요.
2. Ubuntu: ubuntu/WayWin-Setup.desktop의 실행을 허용하고 엽니다. 안내창에서 관리자 인증을 진행하면 필요한 패키지 설치·빌드·앱 설치를 수행합니다. 인터넷이 필요합니다.
3. Windows: windows/WayWin Companion.exe를 실행하고 Install + start at sign-in을 누릅니다. Windows RDP 설정은 지원되는 정상 구성으로 준비하세요.
4. Ubuntu 앱 목록의 WayWin → 호스트 추가 → 전체 데스크톱 연결을 누릅니다. 인증서 이름·지문을 확인합니다.
5. Windows에서 WayWin Windows Probe.exe와 사용할 앱을 실행합니다.
6. WayWin에서 개별 창으로 전환하고 연결 탭의 창·트레이 조작, Windows 앱·셸·클립보드 탭의 버튼을 확인합니다.
7. 실환경 검증 탭에서 P01–P10·셸 3종·M01–M06을 기록하고 보고서를 내보냅니다.

Python 명령은 필요하지 않습니다. 자세한 절차·실패 시 복구는 PRODUCT-GUIDE.md에 있습니다.

이 묶음은 개발 인수 검증용입니다. Windows x64 실행 파일은 교차 컴파일됐으며 실제 Windows 실행은 미확인입니다. Ubuntu는 대상 PC에서 빌드하는 설치 묶음이며 사전 빌드된 .deb가 아닙니다. 실제 RDP·원본 셸·키링·혼합 DPI·8시간 사용·20회 복구 결과는 아직 없습니다.
""")
    hashes = []
    for path in sorted(DEST.rglob("*")):
        if path.is_file():
            hashes.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.relative_to(DEST).as_posix())
    (DEST / "SHA256SUMS").write_text("\n".join(hashes) + "\n")
    archive = DEST.with_name(DEST.name + ".tar.gz")
    with tarfile.open(archive, "w:gz") as output:
        output.add(DEST, arcname=DEST.name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + ".sha256").write_text(digest + "  " + archive.name + "\n")
    print(f"Review bundle: {archive} ({archive.stat().st_size} bytes)")
    print(f"SHA256: {digest}")


if __name__ == "__main__":
    main()
