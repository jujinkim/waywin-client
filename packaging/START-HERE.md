# WayWin 0.3 · 직접 확인하기

1. 이 묶음을 Linux 클라이언트와 Windows RDP 대상에 복사하세요. GNOME·KDE 등의 Wayland/X11에서 사용하며, VM에서도 테스트를 시작할 수 있습니다.
2. Ubuntu/Debian 계열: ubuntu/WayWin-Setup.desktop의 실행을 허용하고 엽니다. 관리자 인증 후 패키지 설치·빌드·앱 설치를 수행합니다. 인터넷과 Qt 6.4 이상·SDL3·SDL3_ttf 개발 패키지를 제공하는 저장소가 필요합니다. 패키지 목록은 Ubuntu 26.04 기준입니다. 다른 Linux는 PRODUCT-GUIDE.md의 개발자 빌드 절차를 따릅니다.
3. Windows: windows/WayWin Companion.exe를 실행하고 Install + start at sign-in을 누릅니다. Windows RDP/RemoteApp은 지원되는 정상 구성으로 준비하세요.
4. Linux 앱 목록의 WayWin → 호스트 추가 → 전체 데스크톱 연결을 누릅니다. **이 PC의 화면 연결**은 기본 자동 감지이며 Wayland 또는 X11 / XWayland를 직접 선택할 수도 있습니다. 인증서 이름·지문을 확인합니다.
5. Windows에서 WayWin Windows Probe.exe와 사용할 앱을 실행합니다.
6. WayWin에서 개별 창으로 전환하고 연결 탭의 창·트레이 조작, Windows 앱·셸·클립보드 탭의 버튼을 확인합니다.
7. 실환경 검증 탭에서 P01–P10·셸 3종·M01–M06을 기록하고 보고서를 내보냅니다. 실제 SDL 드라이버와 데스크톱, 실기기·VM 여부를 구분해 기록하세요. 환경을 바꾸기 전에 기존 보고서를 내보내고 별도 호스트 항목으로 새 검증을 시작하세요.

제품 사용에 Python 명령은 필요하지 않습니다. 자세한 절차·실패 시 복구는 PRODUCT-GUIDE.md에 있습니다.

이 묶음은 개발 인수 검증용입니다. Windows x64 실행 파일은 교차 컴파일됐으며 실제 Windows 실행은 미확인입니다. Linux는 대상 PC에서 빌드하는 설치 묶음이며 사전 빌드된 .deb가 아닙니다. 실제 RDP·원본 셸·키링·혼합 DPI·8시간 사용·20회 복구 및 Linux 데스크톱 호환성은 아직 검증하지 않았습니다.
