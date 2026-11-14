# WayWin 제품 실행과 GUI 검증

2026-11-14 사용자 요청에 따라 제품 구현과 실환경 검증을 병행한다. Qt/C++ 관리 앱과 호스트별 FreeRDP/SDL 프로세스가 실제 접속 및 검증에 같은 경로를 사용한다. **설치 이후 실행·환경 점검·검증 기록에 Python이나 터미널 명령은 필요하지 않다.** 아래 빌드는 개발자/패키지 제작자용이다.

## 빌드

Ubuntu 26.04의 GNOME Wayland에서 다음 개발 패키지를 설치한다.

```sh
sudo apt install build-essential cmake ninja-build git pkg-config \
  qt6-base-dev qt6-base-dev-tools qt6-wayland libqt6keychain1 qtkeychain-qt6-dev \
  libsdl3-dev libsdl3-ttf-dev libssl-dev zlib1g-dev libcjson-dev libjson-c-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev liburiparser-dev libicu-dev
cmake -S . -B build/product -G Ninja -DWAYWIN_BUILD_RUNTIME=ON -DBUILD_TESTING=OFF
cmake --build build/product --parallel 4
sudo cmake --install build/product --prefix /usr/local
```

패키지 이름은 Ubuntu의 [Qt 6](https://packages.ubuntu.com/resolute/qt6-base-dev)와 [QtKeychain 개발 패키지](https://packages.ubuntu.com/resolute/qtkeychain-qt6-dev) 목록을 기준으로 한다.

앱 목록의 **WayWin**을 연다. 개발 빌드는 `build/product/app/waywin` 실행파일을 연다. 최초 빌드에서 CMake가 고정 FreeRDP 커밋을 내려받고 관측 패치와 제품 패치를 순서대로 적용한다. Python 도구나 기존 `.cache/FreeRDP-3.33.1` 작업 사본에 의존하지 않는다. 패치가 바뀌면 새 빌드 디렉터리를 사용한다. 기존 작업 사본은 자동 초기화하지 않는다.

macOS 개발 빌드는 Homebrew `qtbase`, `qtkeychain`, `cmake`, `ninja`, `sdl3`, `sdl3_ttf`, OpenSSL 및 기존 FreeRDP 빌드 의존성이 필요하다. GUI·자동 테스트·컴파일 확인을 지원하며 실제 접속은 Ubuntu 대상 환경에서만 허용한다.

프로브만 빌드할 때에는 `-DWAYWIN_BUILD_APP=OFF`를 지정한다. Windows 시험 앱의 Visual Studio/MinGW 빌드 경로는 기존 `docs/validation.md`와 같다. 현재 Windows 시험 앱은 DVC 제품 보조 앱으로 변경되지 않았다.

## 제품 사용

1. **환경·도움말**에서 환경 점검 결과를 확인한다. 대상 OS, GNOME, Wayland 세션과 소켓을 검사한다. 이 검사는 실제 화면 호환성 합격 판정이 아니다.
2. **호스트 추가**에서 이름·서버 주소·사용자명·인증 도메인을 입력한다. RemoteApp 별칭·화면 ID·창 크기·색 깊이·다중 화면·동적 해상도·소리를 설정할 수 있다. 비밀번호 저장은 선택 사항이다.
3. 전체 데스크톱 또는 개별 창을 선택하고 **연결**을 누른다. 개별 창에서 **등록 앱 새로 실행**을 켜면 등록 앱 실행을 요청한다. 기본은 기존 세션에 연결하는 동작이다. 비밀번호는 제품 입력창 또는 SDL의 추가 인증창에서 입력한다.
4. 인증서 창에서 요청 호스트 이름과 인증서 이름·지문을 확인한다. Enter/Escape의 기본 동작은 취소다. 영구 수락한 인증서는 제품 데이터 폴더의 `certificates/<host UUID>/`에 저장한다. 다른 호스트 및 다른 FreeRDP 클라이언트와 공유하지 않는다.
5. 다른 호스트를 선택해 동시에 접속할 수 있다. 연결 화면의 원격 창 목록에서 활성화·최소화·복원·닫기를 요청하고 알림 아이콘 목록에서 클릭·메뉴 요청을 보낼 수 있다. 아이콘은 현재 원본 그림 대신 `(창 ID, 아이콘 ID)`로 표시한다. RAIL 미지원/정책 차단 시 Windows 서버 설정을 확인한다.
6. **연결 해제**는 로컬 세션 프로세스를 정리한다. **다시 연결**과 **모드 전환**은 앱 실행 요청을 보내지 않는다. 모드 전환은 연결을 해제한 뒤 다른 모드로 재접속한다. Windows의 로그오프·재연결 정책에 따라 같은 세션 보존 여부가 달라진다.
7. 인증이 거부되면 주소·도메인·계정을 확인한다. 저장 비밀번호를 바꾸려면 **검증 도구 → 선택 호스트의 저장 비밀번호 삭제** 후 다시 연결한다. 키링 부재·잠금·실패 시 평문 파일로 저장하지 않고 비밀번호 입력으로 복구한다.

실패 시 원래 호스트를 삭제할 필요 없이 설정을 고치고 다시 연결할 수 있다. 엔진 누락·변조는 **환경·도움말 → 세션 엔진 선택**에서 같은 제품 버전의 `waywin-runtime.json`을 선택하거나 설치를 복구한다. 엔진 검사는 백그라운드에서 실행되어 GUI를 막지 않는다. 명세는 서명되지 않았으므로 해시 검사가 독립적인 공급망 인증을 뜻하지 않는다.

## 제품 안에서 실환경 검증

1. 전체 데스크톱에서 Windows 시험 앱을 실행하고 Session/PID/Instance와 임시 입력을 확인한다.
2. **실환경 검증**에서 P01–P10을 선택한다. 각 항목의 절차와 종료 조건에 따라 제품 연결·원격 창을 조작한다. 원본 셸 3종과 M01–M06 화면 구성도 별도 기록한다.
3. **현재 실행을 선택 검증 항목에 첨부**로 실행 UUID를 붙인다. 실제 관측된 SDL 드라이버가 `wayland`인 경우 드라이버 필드도 채운다. 자동 이벤트만 보고 결과를 합격으로 바꾸지 않는다.
4. 관측·증거 참조·결과, **환경·앱 기록**의 OS/정책/출처·대표 앱 버전 등을 작성한다. 각 호스트의 보고서는 자동 저장된다. 다른 호스트 기록과 섞이지 않는다.
5. **검증 도구** 메뉴에서 선택한 호스트의 일시 정지·재개·강제 종료를 실행해 다른 호스트와 GUI의 반응을 확인한다. 강제 종료는 대상 확인을 받는다.
6. **기록 완결성 확인**으로 누락된 항목을 확인하고 **보고서 내보내기**로 schema 2 JSON을 저장한다. 이 판정은 수동 기록의 완결성만 확인하며 `independently_verified: false`를 유지한다. 내보낸 보고서의 `runs/<UUID>` 참조에 대응하는 실행 폴더도 함께 보관한다.

P04는 원본 아이콘 그림·트레이 수명·클릭·메뉴 전체를, P05는 보조 앱과 작업 보존 전체를 평가한다. 현재 일부 동작을 시험할 수 있다는 이유로 전체 합격을 기록하지 않는다. P09는 Windows 원본 UI의 개별 표시·입력·동일 세션이 필요하며 단순 메뉴 실행 성공은 합격이 아니다.

## 구현 상태와 검증 경계

| 영역 | 현재 구현 | 아직 남은 범위 |
|---|---|---|
| 관리 GUI | 호스트 CRUD, 주소/도메인 분리, 화면/색 깊이/소리 설정, 상태/오류/복구 | 최종 사용성·배율 실환경 검사 |
| 인증 | 비밀번호 비공개 파이프, 선택 OS 키링, 호스트별 인증서 저장, 취소 기본값 | 실제 Windows 인증·인증서 변경·Ubuntu 키링 검증 |
| 세션 | 호스트별 관리/SDL 프로세스, 연결/종료, 수동 재접속, 실행 없는 모드 전환 | 같은 프로세스에서 창을 보존하는 5회 지수 재연결·덮개·세션 교체 감지 |
| RAIL | 창/아이콘 ID 목록, 창 활성화/최소화/복원/닫기, 알림 아이콘 클릭/메뉴 요청 | 원본 아이콘 이미지·제목 표시, 실제 서버 호환성 |
| Windows 연동 | 기존 Windows 시험 앱으로 셸/창/트레이 실험 | 제품 DVC 보조 앱·설치 설정 GUI·원본 셸 미러링 보완 |
| 다중 호스트 | 프로세스/파이프/기록/인증서 범위 분리 | 호스트별 텍스트 클립보드 공유(현재 전체 비활성), 실제 소리/입력 격리 |
| 검증 GUI | 환경 점검, SDL 화면 조회, P01–P10/셸/M01–M06, 자동 저장·가져오기·내보내기·완결성 검사 | 실제 Ubuntu/Windows 결과, 8시간/5앱·20회 복구 |

## 내부 구조와 데이터

- `app/core.*`: 설정·주소·엔진 무결성·환경·이벤트 필터·보고서 계약. Qt Widgets 없이 시험 가능.
- `app/session.*`: 호스트별 세션과 수명 관리. GUI가 같은 실행파일의 `--session-worker`를 실행하고 관리 파이프가 닫히면 해당 SDL 프로세스 그룹만 종료한다. 정지된 그룹은 먼저 재개한다. 3초 유예 뒤 강제 종료한다. Linux는 감시 프로세스 강제 종료 시 직접 SDL 자식도 커널의 parent-death signal로 끝낸다. `setsid`로 이탈한 모든 후손까지 보장하지 않으며 cgroup 전환은 별도 과제다.
- `app/window.*`, `app/validation.*`: 제품 화면과 사용자 관측 기록.
- 제품 런타임 패치: 연결/단절 이벤트, 인증서 저장소/기본 거부, 버전 1 파이프 명령. 명령은 해당 프로세스가 받은 창/아이콘 ID에만 보내며 별도 네트워크 포트는 없다. 명령은 SDL 주 스레드에서 처리한다. 동작 성공은 서버 실제 관측으로 확인한다.
- 설정 `hosts.json`, 실행 `runs/<UUID>/`, 검증 `validation-<host UUID>.json`은 Qt의 앱 로컬 데이터 디렉터리에 둔다. Linux 기본은 `~/.local/share/WayWin/WayWin/`이며 XDG 설정에 따라 달라진다. **기록 폴더 열기**로 실제 위치를 연다. 폴더 0700, JSON/이벤트 0600, 원자적 JSON 교체, 단일 관리자 인스턴스 잠금을 사용한다.
- 비밀번호는 인자·환경 변수·설정·이벤트에 넣지 않는다. 원본 stdout/stderr를 보관하지 않고 허용된 숫자 이벤트만 최대 100,000개 기록한다. 사용자가 작성한 관측 내용은 그대로 저장하므로 민감한 문서 내용은 기록하지 않는다. 자동 업로드는 없다.

## 개발 검증

```sh
cmake -S . -B build/product -G Ninja -DWAYWIN_BUILD_RUNTIME=ON
cmake --build build/product --parallel 4
ctest --test-dir build/product --output-on-failure
```

자동 테스트는 Python 3.10+가 필요한 기존 프로브 검사와 QtTest 제품 검사다. 제품 런타임 자체는 Python을 사용하지 않는다. QtTest는 주소/보안 인자/환경, 기록 권한, 이벤트 크기 제한·민감정보 폐기, 보고서 필수 조건, GUI 호스트 저장/복원, 세션 시작 관문·프로세스 격리·정지 프로세스 회수를 검사한다. macOS와 모의 세션 통과는 Wayland/RDP 지원 증거가 아니다.
