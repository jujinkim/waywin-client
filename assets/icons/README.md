# WayWin 앱 아이콘

짙은 퍼플의 둥근 사각형 안에 오렌지·라벤더 모니터 두 대와 그 사이를
날아가는 창 세 개를 담았다. 부드러운 입체감과 밝은 창으로 원격 창 이동을 표현한다.
글자는 넣지 않았으며 둥근 외곽 밖은 투명하다.

## 산출물

| 파일 | 용도 |
| --- | --- |
| `waywin-source.png` | 내장 이미지 생성 도구의 원본, 1254 × 1254 RGBA |
| `waywin-1024.png` | 1024 × 1024 앱 아이콘 마스터 |
| `waywin-{size}.png` | 16, 24, 32, 48, 64, 128, 256, 512px PNG |
| `waywin.ico` | 16, 24, 32, 48, 64, 128, 256px의 32비트 RGBA 프레임 |

내장 `image_gen`으로 생성했으며 API/CLI 대체 경로는 사용하지 않았다.
정확한 생성 입력과 옵션은 [prompt.md](prompt.md)에 기록했다. 재생성 결과는 달라질 수
있으므로, 동일한 내보내기에는 보관된 원본을 사용한다.

Windows 시험 앱은 ICO를 실행 파일의 리소스로 포함하고, 메인 창과 문서 창에
큰 아이콘·작은 아이콘을 등록한다. 트레이 시험의 정보·경고 아이콘은 별개다.
두 크기는 공유 캐시 없이 따로 로드하며, 클래스 아이콘 핸들은 프로세스 종료까지
유지한다. 크기를 무시하는 공유 캐시와 종료 시 자원 해제 동작은
[Microsoft LoadImageW 문서](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadimagew)를 따른다.
Qt 제품 GUI는 256px PNG를 창/트레이 아이콘으로 사용한다. Linux 설치는 각 PNG를 hicolor 아이콘 디렉터리에 배치하며 `waywin.desktop`에서 참조한다.

## 크기별 파일 다시 만들기

저장소 루트에서 실행한다. Pillow는 이미지 내보내기 전용이며 앱 빌드나 실행에 필요하지 않다.
원본의 투명도를 보존하고 LANCZOS로 축소한다. 아래 명령은 기존 파생 파일을 덮어쓴다.

```sh
rtk proxy python3.12 -m venv .cache/icon-tools
rtk proxy .cache/icon-tools/bin/python -m pip install Pillow==12.2.0
rtk proxy .cache/icon-tools/bin/python - <<'PY'
from pathlib import Path
from PIL import Image

directory = Path("assets/icons")
with Image.open(directory / "waywin-source.png") as source:
    master = source.convert("RGBA").resize((1024, 1024), Image.Resampling.LANCZOS)
master.save(directory / "waywin-1024.png", optimize=True)
sizes = (16, 24, 32, 48, 64, 128, 256, 512)
for size in sizes:
    master.resize((size, size), Image.Resampling.LANCZOS).save(
        directory / f"waywin-{size}.png", optimize=True)
master.save(directory / "waywin.ico", format="ICO",
            sizes=[(size, size) for size in sizes if size <= 256])
PY
```

## 검증 범위

PNG 크기·투명 모서리·ICO 프레임을 검사하고 밝은/어두운 배경에서 축소본을 확인한다.
MinGW 교차 빌드와 실행 파일의 아이콘 리소스 포함 여부는 macOS에서 검사할 수 있다.
실제 Windows 탐색기·작업표시줄·창의 표시 및 혼합 DPI 동작은 Windows에서 별도 확인이 필요하다.
