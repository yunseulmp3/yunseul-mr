# Yunseul MR (윤슬 MR)

노래방 MR(반주)을 자동으로 다듬어 주는 OBS Studio 오디오 필터입니다.
그 순간 주변 대역보다 튀어나온 소리(쏘는 신스, 웅웅대는 저중역, 찢어지는 하이햇)만 찾아서 그만큼만 눌렀다 놓습니다.

An OBS Studio audio filter that automatically smooths harsh or boomy resonances in karaoke backing tracks.

- 지연 0 (zero latency) — 방송에서 MR이 목소리보다 늦어지지 않습니다.
- 노브 하나: **양 (Amount)**. 0 %면 원본 그대로 통과합니다.
- 확인용 스위치: **깎이는 소리만 듣기** — 필터가 무엇을 덜어내는지 들어볼 수 있습니다.

## 쓰는 법

1. OBS에서 **MR만 지나가는 오디오 소스**의 필터를 엽니다. (목소리가 섞인 소스에 걸면 목소리도 같이 깎입니다.)
2. `+` → **윤슬 MR** 을 추가합니다.
3. **양**을 50 %에서 시작해 귀로 맞춥니다.

## 동작 원리

- 60 Hz ~ 16 kHz를 1/8 옥타브 간격 대역 필터 65개로 나눕니다. 필터 계수는 고정이라 소리가 흔들리지 않습니다.
- 약 0.7 ms마다 각 대역의 크기를 좌우 한 옥타브 이웃 평균과 비교합니다. 이웃보다 2.5 dB 넘게 튀어나온 만큼만 줄입니다.
- 비교가 상대적이라 입력 음량과 무관하게 똑같이 동작합니다.
- 출력 = 입력 − Σ(줄일 양 × 대역 신호). FFT를 쓰지 않아 지연이 없습니다.

핵심은 `src/ys_mr_core.h` 한 파일이고 OBS와 무관하게 컴파일됩니다.

## 빌드

[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) 구조 그대로입니다. GitHub Actions가 Windows / macOS / Ubuntu 빌드를 만듭니다.

오프라인 시험:

```
g++ -std=c++17 -O2 -o bin/render tools/render.cpp
python3 tests/synth_test.py bin/render work
```

## 라이선스

GPL-2.0-or-later. `LICENSE` 참고.
이 플러그인은 OBS Project와 제휴하거나 OBS Project가 만든 것이 아닙니다.
This plugin is not affiliated with or developed by the OBS Project.
