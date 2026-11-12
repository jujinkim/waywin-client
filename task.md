# 작업 기록

## 목표·범위

승인된 WayWin 기획을 저장하고 0단계 핵심 가능성 검증을 준비한다. R1–R7과 안정성 기준을 보존한다. 2026-11-13 사용자가 실제 Ubuntu/Windows에 지금 접근 불가라고 확인했으므로 이번 단위는 검증 도구·절차 준비이다. 제품 구현·실환경 검증 완료로 표시하지 않는다.

## 진행

- [x] 빈 Git 저장소·PC 지침·필수 웹 지침(v1 revision 8) 재확인.
- [x] 승인 기획·프로젝트 기준·작업 기록 저장, Apache-2.0 채택.
- [x] 기존 `origin`을 `git@github.com:jujinkim/waywin-client.git`으로 설정. `ls-remote --heads` 성공, 브랜치 없음. 푸시 없음.
- [x] 고정 FreeRDP 소스·단일 파일 관측 패치·빌드 및 검증 실행 도구.
- [x] Windows 창·팝업·트레이 시험 앱 소스와 x64 교차 빌드 산출물.
- [x] 자동 테스트·로컬 빌드·보안 출력 검사, 실환경 GUI 절차와 증거 양식.
- [ ] 0단계 실제 Ubuntu 26.04 GNOME Wayland / Windows 11 검증.
- [ ] 0단계 통과 후 1–3단계 구현.

## 구현 결과

- `tools/probe.py`: 커밋·패치 검증, 기존 수정 보존, SDL3 빌드·해시 명세, Ubuntu/GNOME/Wayland 사전 검사, IP/DNS/IPv6/RDP 주소 검증, 호스트별 프로세스 실행·제한된 종료 대기, UUID별 이벤트 기록, 수동 보고서 완결성 검사.
- 비밀번호 옵션·임의 FreeRDP 인자·인증서 검증 해제 옵션 없음. SDL 인증 GUI를 사용한다. 파일에는 고정 이벤트와 숫자 ID만 쓰고 원본 진단 로그·입력·제목은 버린다. 인자 오류도 값을 다시 출력하지 않는다. 기록 0600/실행 디렉터리 0700, 이벤트 저장 상한 적용.
- 관측 패치는 `client/SDL/SDL3/sdl_rail.cpp` 한 파일에만 적용. 인증·영상·RDP 프로토콜·창 배치 알고리즘 변경 없음. 알림 아이콘 콜백은 **수신 관측만** 하며 표시·입력 구현이 아니다.
- Windows 네이티브 GUI 시험 앱: 문서·팝업·트레이 조작, 지연 창 생성, Notepad 실행, 임시 텍스트, 세션/PID/인스턴스/경과 시간. 네트워크·DVC·RDP 설정 변경 없음.
- `docs/validation.md`에 플랫폼별 빌드, GUI 검증 항목/종료 조건, 실패/차단 판단, 증거·복원 절차. `docs/feasibility-report.example.json`은 전 항목 미실행.

## 검증 결과 (2026-11-13)

| 검사 | 실제 결과·한계 |
|---|---|
| Python 3.12 단위·계약·프로세스 테스트 | **21개 통과**. 잘못된 주소/비밀번호 인자 비노출, 로그 필터·길이 제한, 파일 권한·덮어쓰기 거부, 실 Wayland 소켓 사전 확인, 보고서 누락/실패 차단, 실행파일 변조 거부 |
| 하위 프로세스 통합 테스트 | 합성 세션 2회 실행으로 UUID·시퀀스 분리, 원본 로그 폐기, 상속된 파일 로깅 설정 제거 확인. 실제 RDP 검증 아님 |
| 장애 주입 | 별도 테스트 프로세스 A에 SIGSTOP 후 종료 제한 시간·강제 회수 확인, B 생존 확인. Qt GUI/실제 RDP의 격리 합격을 뜻하지 않음 |
| CMake / CTest | `cmake -S . -B build/checks -G Ninja`, `ctest --test-dir build/checks --output-on-failure` 통과 |
| FreeRDP 빌드 | macOS arm64에서 고정 버전+패치 빌드·링크, `/version` 3.33.1 확인. 패치 갱신 후 재빌드 성공. 빌드 명세의 실행파일 해시 검증 성공 |
| 의존 소스 재실행 | `prepare` 재실행 성공. 승인한 패치와 실제 diff 일치, 중복 적용 안 함 |
| Windows 시험 앱 | MinGW-w64/GCC 16.2.0 x64 교차 빌드, `-Wall -Wextra -Wpedantic -Werror` 통과. PE의 Windows GUI subsystem·시스템 DLL import 확인. Windows 실행/MSVC 빌드 미실행 |
| 환경·진행 조건 | `doctor`는 macOS에서 종료 코드 2/target_ready=false. 빈 예제 보고서 `assess`는 종료 코드 2/record_complete=false. 정상적인 미통과 결과 |
| 비밀정보·변경 검사 | WayWin 소스/문서만 복사한 임시 경로에 Gitleaks 실행 통과. `git diff --check` 통과. 의존성/빌드 전체 대상 초기 스캔은 중단하고 작업 파일로 범위를 제한 |
| 실제 화면·네트워크 | Ubuntu GNOME Wayland, Windows 11, RDP 로그인·인증서·창/트레이·작업 보존·20회 복구·8시간 사용 **전부 미실행**. Computer-use 사용 없음 |

초기 검사에서 Apple 기본 Python 3.9의 API 부족, 테스트 임시 경로의 `/private` 정규화, Windows COM 헤더 누락을 발견해 수정했다. Python 3.10 이상을 명시하고 설치된 3.12로 검증했다. FreeRDP upstream의 deprecated/const 경고, 선택 uriparser 미설치 경고, 중복 라이브러리 링크 경고가 남아 있다. macOS 컴파일 성공을 제품 안정성 통과로 해석하지 않는다.

## 환경·차단 사항

- macOS arm64, Apple Clang 21, SDL 3.4.18. 작업 시작 시 CMake·Qt·Linux 실행 환경 없음.
- 개발 검증을 위해 Homebrew로 CMake 4.4.5, Ninja 1.13.2, SDL3_ttf 3.2.2, MinGW-w64 14.0.0_3를 설치했다. Homebrew가 종속 패키지 설치와 자동 캐시/구버전 정리도 수행했다. 추가 에이전트 스킬은 설치·호출하지 않았다.
- FreeRDP 빌드에서 감지한 주요 패키지: SDL3 3.4.18, SDL3_ttf 3.2.2, OpenSSL 4.0.3, zlib 1.2.12, cJSON 1.7.19. 전체 의존성 고정/배포 SBOM은 아니며 CMake 캐시·명세를 빌드 디렉터리에 남겼다.
- FreeRDP 3.33.1 태그의 커밋 `65bcca1913a28cbdd7eb913599f692bd2b707991` 확인. 원본은 무시되는 `.cache/`에 내려받음.
- SDL3 RAIL 소스는 스스로 experimental로 표시한다. 창 콜백은 있지만 알림 아이콘 콜백은 등록되지 않는다. 규격 지원을 제품 지원으로 간주할 수 없다.
- 실제 Windows 빌드·RDP 정책·기존 창 발견·트레이·세션 보존·복구·장시간 안정성은 미검증이다.
- **0단계 진행 조건 미충족:** 현재 관측 패치로 트레이 표시/조작(P04)은 제공되지 않는다. DVC 보조 앱과 제품 모드 전환(P05)도 미구현이다. 관측 성공·GUI 시험 앱 실행·보고서 형식 검사를 이 조건의 통과로 기록하지 않는다.
- 사용자 신원으로 커밋하지 않음: Git 작성자 설정이 없어 로컬 커밋에는 명시적 자동화 작성자 `Codex <codex@localhost>`를 사용한다.

## 검증·인수인계

관련 체크리스트의 요구 보존·의존성·비밀정보·검증 구분·작업 저장 항목을 변경 전후 대조했다. 문서 단위는 `e1eb3e0`, 검증 도구 단위는 `9c16041`에 로컬 커밋했다. 새 패치 파일의 필수 문맥 공백을 Git whitespace 검사가 오류로 보고하여 `.gitattributes`에서 패치 파일에만 예외를 적용했다. 패치 바이트/해시는 유지하고 적용된 FreeRDP 소스의 `git diff --check`를 별도로 통과했다. 이 메타데이터 보완도 로컬 커밋한다. 빌드 산출물·실행 기록·상위 의존 소스는 Git에서 제외한다. `origin`을 유지하며 푸시·원격 변경·게시는 하지 않았다.

로컬 산출물:

- FreeRDP: `build/freerdp-probe/client/SDL/SDL3/sdl3-freerdp`, 출처 `build/freerdp-probe/build-manifest.json` (macOS용).
- Windows 시험 앱: `build/windows-probe/windows/probe/waywin-windows-probe.exe` (x64, 실행 미검증).
- 절차: `docs/validation.md`, 빈 증거 양식: `docs/feasibility-report.example.json`.

다음 순서: Ubuntu에서 동일 프로필 빌드 → Windows 에디션/빌드·정상 RAIL 허용 경로 확인 → P01–P03과 서버 트레이 이벤트 조사 → 증거에 따라 트레이·세션 보존의 0단계 실험 보완 → 핵심 조건의 실제 통과 후 Qt 관리 GUI·키링·호스트 IPC·제품 DVC 보조 앱 구현. 현재 제품 전체가 완성된 상태가 아니다.
