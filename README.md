# WayWin

Windows RDP 사용자 세션의 창과 셸 기능을 Ubuntu GNOME Wayland에 통합하는 클라이언트입니다. 안정성, 호스트별 프로세스 격리, GUI에서의 복구를 우선합니다. 라이선스는 Apache-2.0입니다.

**현재 상태: 0단계 가능성 검증 도구 준비. 제품 클라이언트는 아직 구현되지 않았습니다.** 실제 Windows 11 / Ubuntu 26.04 검증을 통과하기 전에 관리 GUI와 제품 기능 구현으로 넘어가지 않습니다. macOS의 빌드 성공은 Wayland 호환성 증거가 아닙니다.

- [기획·요구사항·결정](docs/plan.md)
- [채택 기준과 검증 방법](docs/project-checklist.md)
- [현재 작업·검증 결과·다음 단계](task.md)

최종 구조는 Qt 6/C++ 관리 GUI, 호스트별 FreeRDP/SDL 3 세션 프로세스, 같은 저장소의 Windows 보조 앱입니다. 서버 주소와 인증 도메인을 분리하고, 비밀번호는 선택한 경우에만 OS 키링에 저장합니다. 키링 장애 시 평문 파일로 대체하지 않습니다.

GitHub: `git@github.com:jujinkim/waywin-client.git`. 이 작업에서는 로컬 커밋만 수행합니다.
