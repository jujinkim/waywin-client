# WayWin

Windows RDP 사용자 세션의 창과 셸 기능을 Ubuntu GNOME Wayland에 통합하는 클라이언트입니다. 안정성, 호스트별 프로세스 격리, GUI에서의 복구를 우선합니다. 라이선스는 Apache-2.0입니다.

**현재 상태: 0단계 가능성 검증 도구 준비. 제품 클라이언트는 아직 구현되지 않았습니다.** 실제 Windows 11 / Ubuntu 26.04 검증을 통과하기 전에 관리 GUI와 제품 기능 구현으로 넘어가지 않습니다. macOS의 빌드 성공은 Wayland 호환성 증거가 아닙니다.

- [기획·요구사항·결정](docs/plan.md)
- [채택 기준과 검증 방법](docs/project-checklist.md)
- [현재 작업·검증 결과·다음 단계](task.md)
- [0단계 빌드·실행·GUI 검증 절차](docs/validation.md)

검증 준비물은 고정 FreeRDP의 이벤트 관측 패치와 실행 도구, Windows 창·트레이 시험 앱입니다. Python 3.10 이상에서 `python3 tools/probe.py --help`로 시작합니다. 자동 테스트는 `cmake -S . -B build/checks -G Ninja` 후 `ctest --test-dir build/checks --output-on-failure`로 실행합니다. 의존성·플랫폼별 빌드 방법과 결과 기록 방법은 위 검증 안내에 있습니다.

추가 검증 범위는 원본 시작 메뉴·IME 설정·빠른 설정의 개별 미러링과 호스트/클라이언트 멀티모니터입니다. 시험 앱의 셸 실행 메뉴와 도구의 모니터 선택 기능은 이 동작을 조사하기 위한 준비물이며, 실제 미러링이나 멀티모니터 호환성이 입증된 것은 아닙니다.

최종 구조는 Qt 6/C++ 관리 GUI, 호스트별 FreeRDP/SDL 3 세션 프로세스, 같은 저장소의 Windows 보조 앱입니다. 서버 주소와 인증 도메인을 분리하고, 비밀번호는 선택한 경우에만 OS 키링에 저장합니다. 키링 장애 시 평문 파일로 대체하지 않습니다.

GitHub: `git@github.com:jujinkim/waywin-client.git`. 이 작업에서는 로컬 커밋만 수행합니다.
