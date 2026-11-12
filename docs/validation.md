# 0단계 검증 실행 안내

이 도구는 실제 환경의 가능성을 조사하기 위한 개발용 도구이다. 제품의 Qt 관리 GUI, 키링, IPC, DVC 보조 앱, 트레이 UI, 모드 전환을 구현한 것으로 취급하지 않는다. 목표는 R3–R7의 위험을 실제 Ubuntu/Windows에서 확인할 수 있게 하는 것이다.

## 구성과 변경 범위

- `tools/probe.py`: 소스 검증·빌드, 환경 점검, 호스트별 별도 SDL3 프로세스 실행, 제한된 이벤트 수집, 수동 결과 기록 검사. Python 3.10 이상, 표준 라이브러리만 사용한다.
- `patches/freerdp-3.33.1-probe.patch`: SDL3 `sdl_rail.cpp` 하나에만 관측 코드를 추가한다. `WAYWIN_PROBE_EVENTS=1`일 때만 동작한다. 창 생성/삭제, 알림 아이콘 생성/변경/삭제, 모니터링 전환, SDL 드라이버를 기록한다. **트레이 렌더링이나 클릭 전달은 구현하지 않는다.**
- `windows/probe`: 네이티브 Win32 GUI 시험 앱. 세션 ID·PID·인스턴스 UUID·경과 시간·세션 화면 수/가상 영역·DPI, 편집 가능한 임시 텍스트, 문서 창 생성/지연 생성/최소화/복원/크기 변경, 모달 팝업, 트레이 생성/변경/삭제/클릭/메뉴, Notepad와 실제 셸 UI 열기를 제공한다. 네트워크 수신·자동 시스템 설정 변경·키 입력 기록을 하지 않는다. 조작은 메뉴바의 Documents / Tray / Windows shell에 있다.
- `cmake/FreeRDPProbe.cmake`: 시험용 빌드 프로필. 영상 최적화·Kerberos·장치 공유 기능 일부를 끈 기본 경로이다. 이 프로필로 최종 성능·도메인 인증·음성 기능을 평가하지 않는다.

Windows 시험 앱은 최종 보조 앱이 아니다. Notepad와 Windows shell 메뉴는 해당 사용자 세션에서 앱/UI를 실행하는 기초 시험이다. DVC 통신, 설치/설정 UI, 원격 명령 수신은 아직 없다. 메뉴 호출 성공은 해당 UI가 RAIL로 미러링되었다는 뜻이 아니다.

## 빌드와 출처

FreeRDP는 `dependencies.lock.json`의 태그와 커밋을 모두 검사한다. 원본은 `.cache/FreeRDP-3.33.1`, 결과는 `build/freerdp-probe`에 둔다. 예상한 패치 이외의 변경이 있으면 중단하며 기존 변경을 지우지 않는다. 기존 시스템 FreeRDP를 덮어쓰지 않는다.

Ubuntu 26.04 준비 명령(이번 macOS 세션에서는 실행하지 않음):

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git python3 pkg-config \
  libsdl3-dev libsdl3-ttf-dev libssl-dev zlib1g-dev libcjson-dev libjson-c-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev liburiparser-dev
python3 tools/probe.py build
```

배포판의 [SDL3 개발 패키지](https://packages.ubuntu.com/resolute/libsdl3-dev)와 [SDL3_ttf 개발 패키지](https://packages.ubuntu.com/resolute/libsdl3-ttf-dev)를 사용한다. 패키지 버전을 임의로 최신 버전으로 가정하지 않는다. SDL/시스템 라이브러리는 OS 패키지를 사용하므로 이 빌드는 완전히 격리된 재현 빌드가 아니다. `build-manifest.json`에 FreeRDP 커밋·패치/프로필/실행파일 해시·주요 의존성 버전·CMake 캐시 해시를 남기며, 재현에는 같은 시스템 패키지와 툴체인도 필요하다. Ubuntu에서는 다음 정보를 결과와 함께 보관한다.

```sh
dpkg-query -W libsdl3-dev libsdl3-ttf-dev libssl-dev zlib1g-dev libcjson-dev libjson-c-dev
cmake --version
c++ --version
```

macOS에서는 CMake, Ninja, SDL3, SDL3_ttf, OpenSSL 개발 파일이 필요하다. 기본 `python3`가 Apple 제공 3.9라면 설치된 3.10 이상 인터프리터의 경로를 사용한다. macOS 빌드는 소스·링크 검증만 수행하며 `run`은 이 환경을 거부한다.

Windows 시험 앱은 Visual Studio의 C++ 개발 환경과 CMake에서 빌드한다. Python은 검증 도구·자동 테스트를 실행할 때 필요하다.

```powershell
cmake -S . -B build/windows-native -A x64 -DBUILD_TESTING=OFF
cmake --build build/windows-native --config Release
```

실행파일은 `build/windows-native/windows/probe/Release/waywin-windows-probe.exe`이다. macOS/Linux에 MinGW-w64가 있으면 다음 교차 빌드도 가능하다. 이것은 Windows 실행 검증이 아니다.

```sh
cmake -S . -B build/windows-probe -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake -DBUILD_TESTING=OFF
cmake --build build/windows-probe
```

교차 빌드 산출물은 `build/windows-probe/windows/probe/waywin-windows-probe.exe`이다. 자동 테스트:

```sh
cmake -S . -B build/checks -G Ninja
ctest --test-dir build/checks --output-on-failure
```

이 PC에서 에이전트가 실행할 때에는 공통 지침에 따라 위 명령을 `rtk` 또는 `rtk proxy`로 감싼다. RTK는 시험 도구 자체의 런타임 의존성은 아니다.

## 접속 전 확인

1. Ubuntu 설정에서 GNOME Wayland로 로그인했는지 확인한다. `python3 tools/probe.py doctor`가 성공해야 한다. OS·세션 환경과 Wayland 소켓 존재를 검사할 뿐, 실제 SDL 드라이버나 RDP 호환성을 증명하지 않는다.
2. Windows 설정의 에디션·전체 OS 빌드, 원격 데스크톱 설정·사용자 권한, 적용된 RemoteApp 정책·앱 별칭을 기록한다. 일반 Windows 11에서 모든 창의 RAIL 전송이 된다고 가정하지 않는다. 표준 설정으로 RemoteApp를 허용할 수 없다면 해당 시험을 `blocked`로 기록한다. 정책·앱 등록을 자동으로 바꾸는 스크립트는 제공하지 않는다.
3. 시험 앱을 원하는 RDP **사용자 세션**에서 실행한다. 물리 콘솔 공유를 시험하지 않는다. Session/PID/Instance를 기록하고 임시 편집칸에 무관한 시험 문구를 입력한다. UUID는 프로세스가 다시 실행됐는지 확인하는 표식이다. 서로 다른 인스턴스가 있으면 중복 실행으로 기록한다.
4. 시험 호스트에는 `host-a`, `host-b` 같은 별칭을 사용한다. 비밀번호를 명령행·보고서·채팅에 넣지 않는다. 비밀번호는 SDL GUI 대화상자에서 입력한다. 인증서 확인은 기존 SDL GUI를 사용하며 신뢰 여부는 실제 인증서와 대조한다. 검증 해제 옵션은 제공하지 않는다.

## 실행

GNOME 데스크톱의 터미널에서 실행한다. 서버 주소와 Windows 인증 도메인은 별도 옵션이다. DNS/IP/포트, `rdp://host:port`, `[IPv6]:port`를 허용하며 URL 사용자정보·경로·쿼리는 거부한다.

```sh
python3 tools/probe.py run --server rdp://windows.example:3389 \
  --user alice --domain WORKGROUP --label host-a --mode desktop
python3 tools/probe.py run --server windows.example \
  --user alice --label host-a --mode rail --app '||WayWinProbe'
```

`||WayWinProbe`는 서버에 실제로 등록·허용한 별칭으로 바꾼다. 서버가 허용한 경우 실행파일의 절대 Windows 경로도 사용할 수 있다. 경로/별칭을 넣었다는 것만으로 Windows의 RAIL 지원이 생기지 않는다. 시험 앱 실행 요청은 이미 있는 인스턴스를 재사용한다고 가정하지 않는다.

출력의 `Session process`는 해당 실행의 로컬 SDL 프로세스 PID이다. 인증 GUI 취소 또는 터미널 Ctrl-C로 해당 접속을 끝낸다. 반응이 없는 프로세스는 종료 요청 후 3초가 지나면 강제 종료한다. Windows 로그오프 명령은 보내지 않는다. 원격 세션의 단절 시 로그오프 정책은 서버에 따라 다르므로 반드시 기록한다. 자동 재연결은 이 시험에서 꺼져 있다.

각 실행은 UUID 폴더에 결과를 나누며 RDP 주소·계정·비밀번호·창 제목·키 입력·아이콘 비트맵을 저장하지 않는다. 사용자가 정한 호스트 별칭은 기록한다. 원본 stdout은 버리고 stderr에서는 정해진 이벤트 형식만 저장한다. 잘못 넣은 인자를 오류 메시지에 다시 출력하지 않는다. 실패 원인의 자세한 문구는 SDL GUI에서 확인해 민감정보 없이 수동 기록한다.

### 모니터 선택과 변경 시험

`list-monitors`는 서버에 연결하지 않고 해당 SDL3 빌드에서 감지한 **클라이언트** 화면 ID를 출력한다. Windows 디스플레이 번호나 호스트의 물리 모니터 번호를 넣지 않는다. ID는 모니터 분리/재연결이나 새 SDL 프로세스에서 바뀔 수 있으므로 다시 조회한다.

```sh
python3 tools/probe.py list-monitors
python3 tools/probe.py run --server windows.example --user alice --label host-a \
  --mode desktop --display-mode multimon --monitors 2,1 --dynamic-resolution
python3 tools/probe.py run --server windows.example --user alice --label host-a \
  --mode rail --app '||WayWinProbe' --monitors 2
```

위 `2,1`과 `2`는 실제 조회값으로 바꾼다. 첫 ID가 요청한 원격 주 모니터이다. `--display-mode auto` 기본값은 데스크톱에서 1280×720 창, RAIL에서 전체 모니터 배치이다. RAIL 한 화면 시험도 `--monitors <하나의 ID>`로 한다. RAIL에 고정 1280×720을 전달하여 단일 모니터라고 오해하지 않도록 바꿨다. 단일 데스크톱 창 크기는 `--size 1920x1080`처럼 지정한다. 모니터 배치 모드와 고정 크기 조합은 거부한다.

`--dynamic-resolution`은 display-control 변경을 시험하는 명시적 옵션이며 기본은 꺼져 있다. 켜졌다는 것만으로 hotplug 지원을 보장하지 않는다. 특히 새 모니터가 선택 목록에 없는데 세션에 추가되는지, 제거된 모니터의 원격 창을 남은 화면에서 복구할 수 있는지 확인한다. `run.json`의 `requested_display`는 **요청값**이며 Windows 시험 앱의 세션 화면 수/영역 및 디스플레이 설정에서 실제 결과를 대조한다.

`out/runs/<UUID>/run.json`, `events.jsonl`, `exit.json`은 로컬 증거이다. 파일 0600, 실행 디렉터리 0700, 이벤트 최대 100,000개이며 상한 도달은 `events_truncated`로 기록한다. 일반 로그를 수집하지 않는다. 데이터는 자동 업로드하지 않고 `.gitignore`에서 제외한다. 로그 수신은 실제 화면·입력 성공 증거가 아니다. `driver=wayland`를 RAIL 실행에서 확인한다. 데스크톱 전용 실행은 RAIL 초기화가 없어 드라이버 관측 이벤트가 없을 수 있다.

## GUI 검증 항목과 종료 조건

한 항목을 짧게 수행하고 성공·실패·차단을 기록한 뒤 종료한다. 실패한 조작을 목적 없이 반복하지 않는다. 각 관측에 실행 UUID, Windows Session/PID/Instance, 앱·버전, 배율, 수행 동작·실제 결과를 붙인다. 자동 화면 녹화·키 입력 기록은 사용하지 않는다.

| ID | 동작 | 필요한 증거·종료 조건 |
|---|---|---|
| P01 | RAIL 창 표시, 드래그, 5회 크기 변경, 한영 입력/조합키, Popup dialog 열기/닫기. 100/150/200%에서 각 반복 | SDL `wayland` 이벤트와 실제 창/포커스/좌표 일치. 오작동 시 해당 배율과 조작을 기록하고 종료 |
| P02 | 데스크톱 세션에서 시험 문서·대표 앱 5개를 먼저 열고 접속 해제. 같은 계정의 RAIL 접속에서 기존 창 확인. New window 및 New in 15 seconds로 새 창 확인 | 접속 전후 인스턴스/문서 식별, 기존/새 창 각각의 목록. 선택 앱 한 개 성공으로 전체 지원을 선언하지 않음 |
| P03 | 문서를 다른 창으로 가림, 최소화 후 Restore documents, 최대화/일반 복원. 경과 시간·임시 텍스트 확인 | 가림/복원 후 갱신·입력·상태 유지. 새로 생성된 창으로 대체됐으면 실패 |
| P04 | Add/Change/Delete tray icon 각 3회. 아이콘 좌클릭, 우클릭 메뉴의 New probe window 실행 | 로컬 데스크톱에서 생성/변경/삭제/클릭/메뉴 모두 확인. 이벤트만 수신되면 `blocked` 또는 `fail`, pass 금지. 현재 패치는 로컬 트레이 표시/조작을 제공하지 않으므로 P04 전체 통과는 불가 |
| P05 | Notepad 실행 및 같은 사용자 세션의 데스크톱→RAIL→데스크톱 탐색. 각 전환 전후 Session/PID/Instance·입력 내용 비교 | 실행과 모든 창/작업 보존, 추가 실행 없음. 현재 제품 DVC 및 모드 전환 UI가 없어 전체 pass는 불가. 재접속 실험 결과와 완성된 모드 전환을 구별 |
| P06 | 네트워크 일시 단절/복원, 사용자 재접속. 최종 기준은 20회 | 앱 중복 실행·로그오프·회복 불가 창 없음. 인증서 변경/잘못된 인증/도달 불가도 각각 기록. 의도한 Windows 앱은 닫지 않고 RDP 접속만 해제 |
| P07 | 두 호스트를 별도 터미널에서 동시에 접속. A 프로세스를 정지/재개/종료하는 동안 B에서 입력/창 조작 | A/B 실행 UUID·PID를 확인하고 B가 반응하는지 기록. 이 시험 도구의 격리 결과는 아직 없는 Qt GUI의 장애 격리 통과가 아님 |
| P08 | 같은 세션에서 전체 데스크톱과 선택 창의 혼합·중복 표시 가능성 조사 | 선택 사항. 미지원이어도 0단계 필수 항목을 대체하거나 막지 않음 |
| P09 | Windows shell 메뉴에서 Start menu, Input settings / Korean IME settings, Quick Settings를 각각 열고 선택·입력·닫기 | 각 **원본 Windows UI**가 Ubuntu에 별도로 표시되고 동일 세션에서 조작되어야 함. 아래 세부 기준 모두 확인. URI/메시지/단축키 전송 성공만으로 pass 금지 |
| P10 | 아래 M01–M06 조합에서 문서·셸 UI 이동, 입력, 최대화/복원, 화면 변경 후 회수 | 호스트 물리 화면·클라이언트 화면·협상된 RDP 세션 배치를 모두 기록. 한 조합 성공을 전체 멀티모니터 보장으로 확대하지 않음 |

P07의 정지/종료 주입은 GUI 탐색 대신 출력에서 확인한 정확한 PID에 한해 개발 터미널에서 수행한다. 예: `kill -STOP <A의 PID>`, `kill -CONT <A의 PID>`, `kill -TERM <A의 PID>`. 각 동작 후 B를 짧게 확인한다. 정지한 프로세스는 재개하거나 종료해 남겨 두지 않는다.

8시간/5앱·자원 누적·20회 복구·다중 호스트 클립보드·소리·CLI 없는 GUI 완결성은 최종 제품 합격 기준이다. 현재 프로브로 모두 통과했다고 기록하지 않는다. 정적인 화면 대신 프로세스 응답과 명령 처리로 정지를 판단한다.

### P09: 작업표시줄 없이 필요한 원본 셸 UI

작업표시줄 전체가 없어도 시작 메뉴·IME 설정·빠른 설정의 실제 UI를 개별 전송해야 한다. 데스크톱 모드는 기준 화면을 비교하는 용도로 사용하며 개별 미러링 합격을 대신하지 않는다.

| UI | 시험 앱의 진입점 | 합격에 필요한 관측 |
|---|---|---|
| 시작 메뉴 | Windows shell → Start menu (`SC_TASKLIST`) | 원본 메뉴 표시, 검색 입력·앱 선택, Esc/외부 클릭으로 닫기, 원래 창에 포커스 복귀. 창을 다른 모니터로 옮긴 뒤 재확인 |
| IME | Input settings, Korean IME settings (한국어 IME 설치 시) | 원본 설정 화면 표시·탐색·입력 언어 상태 반영. 시험 문서에서 Windows IME 조합/후보창 위치·선택을 100/150/200%와 화면 경계에서 확인. Ubuntu 로컬 IME가 대신 처리한 결과와 구분 |
| 빠른 설정 | Windows shell → Quick Settings (Win+A) | 실제 Windows 빠른 설정 패널 표시·키보드/포인터 탐색·닫기. 세션에서 이용 가능한 컨트롤 확인. 호스트 정책·하드웨어로 항목이 없는 경우 실제 관측/사유 기록 |

Quick Settings는 시험 앱이 전경에 있고 관련 보조키가 눌리지 않았을 때만 고정 Win+A 이벤트를 보낸다. 키 입력을 수집·저장하지 않는다. 정책/UIPI/셸 상태 때문에 호출이 실패할 수 있다. 시작 메뉴는 [SC_TASKLIST](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand), IME 설정은 [문서화된 설정 URI](https://learn.microsoft.com/en-us/windows/apps/develop/launch/launch-settings), 빠른 설정은 [Windows 단축키](https://support.microsoft.com/en-us/accessibility/windows/windows-keyboard-shortcuts-for-accessibility)를 사용한다. 이 자료는 **열기 방법**의 근거이며 RAIL 미러링 지원 증거는 아니다.

`shell_surfaces`의 start/ime/quick마다 `remote_ui_visible`, `interactive`, `same_session`과 관측/증거를 기록한다. 설정 바로가기 버튼만 동작하거나 데스크톱 안에서만 보인다면 개별 미러링은 `blocked` 또는 `fail`이다. 아직 제품 미러링 경로가 구현·실증되지 않아 세 항목 모두 미검증이다.

### P10: 호스트·클라이언트 모니터 조합

| 항목 | 구성·동작 | 확인할 실패 |
|---|---|---|
| M01 | 호스트 물리 1 / 클라이언트 1, 데스크톱·RAIL 각각 | 기본 좌표·포커스·셸 팝업 |
| M02 | 호스트 물리 2 이상 / 클라이언트 1 | 호스트에 여러 화면이 있어도 RDP 세션의 모든 대상 창을 회수할 수 있는지 |
| M03 | 호스트 물리 1 / 클라이언트 2 이상, 전체 및 부분 선택 | 각 화면의 창 표시·입력·최대화. 호스트 물리 화면 수에 잘못 종속되는지 |
| M04 | 양쪽 2 이상, 서로 다른 해상도/주 모니터 | 실제 RDP 배치와 입력 매핑, 선택하지 않은 클라이언트 화면 침범 |
| M05 | 보조 화면을 왼쪽/위쪽(음수 좌표)에 배치, 세로 회전, 100/150/200% 혼합. 기존 주 화면을 제외한 부분 선택도 수행 | 화면 경계 이동 후 커서·IME 후보창·메뉴 위치, 잘림·흐림·잘못된 최대화, 각 선택 배치의 원격 주 화면 |
| M06 | 정적 설정과 `--dynamic-resolution`을 분리해 hotplug/주 화면 변경/재접속 | 창 유실·검은 창·입력 오프셋·새 ID 혼선·중복 실행·멈춤. 변경 후에도 회수 또는 명확한 복구 경로 필요 |

시험 앱이 보여 주는 Session displays/bounds는 **그 프로세스가 실행되는 사용자 세션**의 값이다. 호스트의 물리 콘솔 모니터 수는 사전에 별도로 기록한다. 보고서 `monitor_matrix`에서 `host_physical_layout`, `client_layout`, `rdp_session_layout`을 모두 채운다. 모니터 개수뿐 아니라 원점·크기·배율·회전·주 화면·선택 ID와 변경 순서를 기록한다.

소스에서 RAIL 다중 화면 처리를 확인했지만, 연결 중 입력 원점 재계산과 선택 목록/hotplug 정합성은 아직 확인되지 않았다. 자세한 소스 조사와 근거는 `docs/plan.md`의 재검토 항목에 있다. 실제 재현 전에는 upstream 렌더링/좌표 코드를 추측으로 수정하지 않는다.

## 결과 기록과 진행 결정

```sh
mkdir -p out
python3 tools/probe.py new-report out/feasibility.json
python3 tools/probe.py assess out/feasibility.json
```

새 양식은 schema 2이며 모두 `not-run`이다. 상태는 `not-run`, `blocked`, `fail`, `pass`로 기록한다. `applications`에 실제 대표 앱/버전 5개 이상, 필수 항목마다 증거 참조와 관측 설명을 넣는다. `environment`의 Windows 에디션/빌드·RDP 정책·GNOME 버전과 FreeRDP 커밋/패치 해시를 채운다. P09/P10과 셸 세 항목·모니터 조합 여섯 항목도 필수다. 이전 schema 1 기록은 자동 합격 처리하지 않는다. 기존 파일을 보존하고 새 양식에 기존 관측을 옮기되 새 항목은 미실행으로 둔다.

`assess`의 성공은 **수동 기록의 필수 필드가 갖추어졌다는 의미**이다. 자동 검증이나 독립적인 사실 확인이 아니며, 출력에도 `independently_verified: false`를 유지한다. 빈 보고서, 실패·차단, 필수 항목 누락, X11 결과는 종료 코드 2이다. 실제 증거를 검토하여 `task.md`에 진행 결정을 기록해야 한다. 현재 P04/P05/P09의 구현 공백과 P10을 포함한 실환경 부재로 0단계 전체는 통과하지 않았다.

## 복원·다음 작업

관측 패치에는 네트워크 프로토콜·인증·렌더링 변경이 없다. 원본과의 비교가 필요하면 별도 디렉터리에 고정 커밋의 원본을 빌드해 동일 절차를 수행하고 관측 없는 결과로 구분한다. 기존 `.cache`에 다른 수정이 있으면 보존한다. 태그만 바꾸거나 이미 설치된 FreeRDP를 이 검증본으로 간주하지 않는다.

아직 검증을 통과한 배포 버전은 없다. 향후 통과 버전은 해당 Git 커밋·의존성 패키지 버전·빌드 명세·바이너리를 함께 보관하고 복원한다. 현재의 이전 상태 복원은 별도 Git checkout과 별도 빌드 디렉터리로 가능하다. 시스템 설치/Windows RDP 설정을 자동으로 되돌릴 대상은 이 도구가 만들지 않는다.

다음 실환경 작업은 Windows 11의 RAIL 허용 경로와 P01–P03 조사이다. 그 증거를 바탕으로 트레이 UI/조작과 같은 세션 보존 경로를 **0단계 실험**으로 구현한다. 핵심 불가 판정이면 증거와 대안을 사용자에게 제시하고, 아키텍처나 요구사항을 임의로 축소하지 않는다.
