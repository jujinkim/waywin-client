#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
umask 077
bundle="$(cd -- "$(dirname -- "$0")" && pwd)"
source_root="$bundle/source"
if [[ ! -f "$source_root/CMakeLists.txt" ]]; then source_root="$(cd "$bundle/.." && pwd)"; fi
prefix="$HOME/.local/waywin"
if [[ "$(uname -s)" != Linux ]] || [[ ! -x /usr/bin/apt-get ]]; then
    message="이 설치 도구는 Ubuntu/Debian 계열의 apt를 사용합니다. 다른 Linux는 docs/product.md의 빌드 안내를 따르세요."
    if command -v zenity >/dev/null; then zenity --error --text="$message"; else printf '%s\n' "$message" >&2; fi
    exit 1
fi
if ! command -v zenity >/dev/null; then pkexec /usr/bin/apt-get install -y zenity; fi
if [[ $# -gt 0 ]] && [[ "$1" == --remove ]]; then
    zenity --question --text="WayWin 실행 파일을 제거할까요? 설정·비밀번호·검증 기록은 보존됩니다." || exit 0
    rm -rf -- "$prefix"
    rm -f -- "$HOME/.local/share/applications/waywin.desktop" "$HOME/.local/share/applications/waywin-remove.desktop"
    zenity --info --text="WayWin 실행 파일을 제거했습니다."; exit 0
fi
runtime_archive="$bundle/dependencies/FreeRDP-3.33.1.tar"
if [[ ! -f "$runtime_archive" ]]; then
    zenity --error --width=520 --text="설치 묶음에 FreeRDP 소스가 없습니다.\n전체 설치 묶음을 다시 받아 압축을 풀어 주세요.\n필요한 파일: $runtime_archive"
    exit 1
fi
zenity --question --width=520 --title="WayWin 설치" --text="필요한 Linux 패키지를 설치하고 WayWin을 빌드합니다.\nFreeRDP 소스는 설치 묶음에 포함되어 GitHub에서 내려받지 않습니다.\nGNOME·KDE 등의 Wayland/X11 화면에서 사용할 수 있습니다.\nOS 패키지 설치에는 인터넷 연결과 관리자 인증이 필요합니다.\n앱은 사용자 폴더에 설치하며 빌드는 시간이 걸릴 수 있습니다." || exit 0
build_id="$(sha256sum "$source_root/patches/freerdp-3.33.1-product.patch" \
    "$source_root/patches/freerdp-3.33.1-probe.patch" "$source_root/dependencies.lock.json" \
    "$source_root/cmake/ProductRuntime.cmake" "$source_root/cmake/ApplyRuntimePatches.cmake" \
    "$source_root/cmake/FreeRDPProduct.cmake" "$source_root/cmake/RuntimeManifest.cmake" | sha256sum | cut -c1-16)"
build_dir="$HOME/.cache/waywin/build-$build_id"
mkdir -p "$build_dir"
log="$build_dir/setup.log"
success="$build_dir/setup-success"
rm -f "$success"
(
    set -e
    pkexec /usr/bin/apt-get update
    pkexec /usr/bin/apt-get install -y build-essential cmake ninja-build git pkg-config \
        qt6-base-dev qt6-base-dev-tools qt6-wayland qtkeychain-qt6-dev \
        libsdl3-dev libsdl3-ttf-dev libssl-dev zlib1g-dev libcjson-dev libjson-c-dev \
        libwayland-dev wayland-protocols libxkbcommon-dev libx11-dev libxext-dev \
        libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev libxcb-cursor0 liburiparser-dev libicu-dev
    cmake -S "$source_root" -B "$build_dir" -G Ninja \
        -DWAYWIN_BUILD_RUNTIME=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
        -DWAYWIN_RUNTIME_ARCHIVE="$runtime_archive"
    cmake --build "$build_dir" --parallel 4
    cmake --install "$build_dir" --prefix "$prefix"
    mkdir -p "$HOME/.local/share/applications" "$prefix/setup"
    cp "$bundle/install-linux.sh" "$prefix/setup/"
    app_path="$(printf '%s' "$prefix" | sed 's/\\/\\\\/g;s/"/\\"/g;s/[$]/\\$/g;s/%/%%/g')"
    cat > "$HOME/.local/share/applications/waywin.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=WayWin
Comment=Windows 작업 공간
Exec="$app_path/bin/waywin"
Icon=$prefix/share/icons/hicolor/256x256/apps/waywin.png
Terminal=false
Categories=Network;RemoteAccess;
ENTRY
    cat > "$HOME/.local/share/applications/waywin-remove.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=WayWin 제거
Exec="$app_path/setup/install-linux.sh" --remove
Icon=$prefix/share/icons/hicolor/256x256/apps/waywin.png
Terminal=false
Categories=Settings;
ENTRY
    touch "$success"
) >"$log" 2>&1 &
job=$!
while kill -0 "$job" 2>/dev/null; do
    echo "# WayWin을 설치하고 있습니다. 관리 권한 요청이 나타나면 인증하세요."
    sleep 2
done | zenity --progress --pulsate --auto-close --no-cancel --width=520 --title="WayWin 설치 중" || true
wait "$job" || true
if [[ ! -f "$success" ]]; then
    zenity --error --width=520 --text="설치에 실패했습니다. 기존 설정은 보존됩니다.\n진단 파일: $log"
    zenity --text-info --filename="$log" --width=900 --height=600 --title="WayWin 설치 진단" || true
    exit 1
fi
zenity --info --width=480 --text="설치했습니다. 앱 목록에서 WayWin을 여세요.\nWindows에서 함께 제공된 WayWin Companion.exe도 실행하세요."
exec "$prefix/bin/waywin"
