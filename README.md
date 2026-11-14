# WayWin

Windows RDP 사용자 세션의 창과 셸 기능을 Ubuntu GNOME Wayland에 통합하는 클라이언트입니다. 안정성, 호스트별 프로세스 격리, GUI에서의 복구를 우선합니다. 라이선스는 Apache-2.0입니다.

**현재 상태: Qt/C++ 제품 GUI와 네이티브 접속·검증 경로 구현. 실제 Ubuntu/Windows 검증은 미실행입니다.**

제품에서 호스트 관리, 키링 인증, 접속·해제·재접속·모드 전환, 원격 창/알림 아이콘 ID 조작, 환경 점검과 검증 보고서 작성을 진행할 수 있습니다. 설치 후 Python 스크립트를 실행할 필요가 없습니다. 원본 트레이 그림, DVC 보조 앱, 창을 유지하는 자동 재연결 등 남은 기능은 [제품 상태표](docs/product.md)에 명시합니다. macOS 빌드와 모의 테스트는 Wayland 호환성 증거가 아닙니다.

- **[제품 빌드·GUI 사용·실환경 검증](docs/product.md)**

- [기획·요구사항·결정](docs/plan.md)
- [채택 기준과 검증 방법](docs/project-checklist.md)
- [현재 작업·검증 결과·다음 단계](task.md)
- [0단계 빌드·실행·GUI 검증 절차](docs/validation.md)

검증 준비물은 고정 FreeRDP의 이벤트 관측 패치와 실행 도구, Windows 창·트레이 시험 앱입니다. Python 3.10 이상에서 `python3 tools/probe.py --help`로 시작합니다. 자동 테스트는 `cmake -S . -B build/checks -G Ninja` 후 `ctest --test-dir build/checks --output-on-failure`로 실행합니다. 의존성·플랫폼별 빌드 방법과 결과 기록 방법은 위 검증 안내에 있습니다.

추가 검증 범위는 원본 시작 메뉴·IME 설정·빠른 설정의 개별 미러링과 호스트/클라이언트 멀티모니터입니다. 시험 앱의 셸 실행 메뉴와 도구의 모니터 선택 기능은 이 동작을 조사하기 위한 준비물이며, 실제 미러링이나 멀티모니터 호환성이 입증된 것은 아닙니다.

현재 구조는 Qt 6/C++ 관리 GUI와 호스트별 FreeRDP/SDL 3 세션 프로세스입니다. 같은 저장소의 Windows 시험 앱을 유지하며 제품 DVC 보조 앱은 남은 구현 범위입니다. 서버 주소와 인증 도메인을 분리하고, 비밀번호는 선택한 경우에만 OS 키링에 저장합니다. 키링 장애 시 평문 파일로 대체하지 않습니다.

GitHub: `git@github.com:jujinkim/waywin-client.git`. 원격 푸시는 명시적 승인이 있을 때만 합니다.
