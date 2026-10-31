# Manny Motion In-betweening

Unreal Engine 5.8 에디터에서 두 Manny 동작 사이를 공식 Context + Detail 모델로 생성하고 Animation Sequence로 저장합니다. Windows · UE 5.8.3 · Python 3.11에서 검증했습니다. 모델은 CPU에서 실행하며 새 학습은 필요 없습니다.

## 저장소 가져오기와 빌드

Git LFS, Unreal Engine 5.8, Unreal C++ 빌드 도구가 필요합니다. 저장소를 받을 폴더에서 PowerShell로 실행합니다.

```powershell
git lfs install
git clone https://github.com/moumee/ue5-transition-anim-generator.git
cd ue5-transition-anim-generator
git lfs pull
```

이미 받은 저장소라면 개인 변경을 먼저 보관한 뒤 `git switch main`, `git pull --ff-only`, `git lfs pull`을 실행합니다. ZIP 다운로드 대신 Git LFS로 받으면 Manny와 데모 에셋 원본을 함께 받습니다.

아래부터는 저장소 루트에서 실행합니다. Unreal을 다른 위치에 설치했다면 엔진 경로를 바꿉니다. 빌드할 프로젝트의 에디터는 닫아 둡니다.

```powershell
$projectFile = (Resolve-Path '.\transition_generator.uproject').Path
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' transition_generatorEditor Win64 Development $projectFile -WaitMutex -NoHotReloadFromIDE
```

빌드가 성공하면 `transition_generator.uproject`를 엽니다.

## 결과만 먼저 재생하기

콘텐츠 브라우저의 `Content/MotionInbetweening/Generated/AN_Manny_Output23`을 열고 ▶를 누릅니다. 이 에셋은 아래 샘플과 공식 모델로 생성한 Walk→Run 결과이며 31키·30fps·1초입니다. 기존 결과 재생에는 외부 Python과 모델 설치가 필요 없습니다.

0~9프레임은 Walk 문맥, 10~29는 생성, 30은 Run 목표입니다. 전환 경계 9→10과 29→30, 진행 방향과 발 움직임을 확인합니다. 30→0 반복 재생 점프는 전환 내부의 경계와 구분합니다. 뷰포트에서 `Alt+H`는 이 샘플의 측면, `Alt+G`는 원근, `F`는 현재 포즈에 카메라 맞추기입니다.

## 내 PC에서 직접 생성하기

### Python과 공식 모델 준비

Python 3.11을 설치한 뒤 저장소 루트에서 실행합니다. 가상환경 활성화는 필요 없습니다.

```powershell
py -3.11 -m venv .venv
& '.\.venv\Scripts\python.exe' -m pip install -r '.\Tools\MotionInference\requirements.txt'
& '.\.venv\Scripts\python.exe' -m pip check
Copy-Item '.\Tools\MotionInference\model_config.example.json' '.\Tools\MotionInference\model_config.local.json'
```

공식 [pre-treained.zip](https://github.com/victorqin/motion_inbetweening/releases/download/v1.0.0/pre-treained.zip)을 받아 다음 네 파일이 이 위치에 오도록 압축을 풉니다. 가중치는 저장소에 포함되지 않으며 실행 때 [고정 해시](Tools/MotionInference/model_manifest.json)를 검사합니다.

```text
Tools/MotionInference/models/
  lafan1_context_model_release/
    checkpoint_lafan1_context_model_release.pth
    train_stats_context.pkl
  lafan1_detail_model_release/
    checkpoint_lafan1_detail_model_release.pth
    train_stats_detail.pkl
```

예제 설정은 위 상대 경로를 그대로 사용합니다. 다른 폴더에 보관하려면 본인의 `model_config.local.json`에서 네 경로를 바꿉니다. 상대 경로는 설정 파일 폴더 기준입니다. `.venv`, 모델, 개인 설정은 Git에서 제외됩니다.

에디터 전에 샘플 추론만 확인할 수 있습니다. `Saved/ModelSmoke`는 아직 없는 폴더여야 하며 다시 실행할 때 새 이름을 씁니다.

```powershell
& '.\.venv\Scripts\python.exe' -X utf8 -B '.\Tools\MotionInference\run_sample.py' --config '.\Tools\MotionInference\model_config.local.json' --output-dir '.\Saved\ModelSmoke'
```

성공하면 `Saved/ModelSmoke/model_output.json`을 생성하고 종료 코드 0을 반환합니다. 이 명령은 JSON까지 생성하며 Unreal 에셋 저장은 아래 메뉴에서 실행합니다.

### Unreal 설정과 생성

**도구 → Motion In-betweening Settings**에서 아래 파일을 현재 PC의 절대 경로로 선택합니다. `<저장소>`는 이 프로젝트를 받은 위치입니다.

| 설정 | 값 |
| --- | --- |
| Python Executable | `<저장소>/.venv/Scripts/python.exe` |
| Inference Python | 같은 Python 또는 빈 값(위 Python 사용) |
| Inference Script | `<저장소>/Tools/MotionInference/infer.py` |
| Model Config | `<저장소>/Tools/MotionInference/model_config.local.json` |
| Training Statistics | 첫 실행은 빈 값. 비정규화 입력을 모델 진입점이 정규화함 |
| Transition Samples | `20` |
| Maximum Samples | `31` |
| Maximum Root Step (cm per sample) | `20` — Validation 고급 항목. Run 직접 내보내기에 필요 |

개인 설정은 `Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini`에 저장합니다. 다른 사람의 절대 경로 설정이나 이 프로젝트 외부의 준비 스크립트는 필요 없습니다.

1. **도구 → Generate Manny Transition from Model Inputs**를 누릅니다.
2. START에서 `<저장소>/Tools/MotionInference/samples/start_input.json`, DESTINATION에서 같은 폴더의 `end_input.json`을 선택합니다. 파일 선택창에 붙여넣을 때는 Windows 역슬래시 경로를 사용합니다.
3. 생성 완료 후 새 `AN_Manny_Output숫자` 미리보기가 열립니다. 자동 저장되며 기존 데모를 덮어쓰지 않습니다. 31키·30fps·1초를 확인합니다.
4. 재생·경계·방향·발 움직임을 확인하고 닫은 뒤 `Content/MotionInbetweening/Generated`에서 다시 엽니다.

다른 입력은 Manny Animation Sequence를 선택하고 **Export Selected Manny Model Input**으로 각각 내보냅니다. `MM_Walk_Fwd`, `MM_Run_Fwd` 원본은 `Content/ControlRig/Characters/Mannequins/Animations/Manny`에 있습니다. 전체 설정과 제한은 [플러그인 안내](Plugins/MotionInbetweeningHello/README.md), 모델 규약은 [추론 안내](Tools/MotionInference/README.md)를 참고합니다.

## 확인 범위와 문제 확인

새 Python 환경 설치, 모델/좌표 16개와 호출기 27개 검사, UE 빌드·실제 추론/에셋 저장·새 프로세스 재로드·전체 메뉴 흐름을 확인했습니다. 2026-10-31 사용자가 Walk→Run 테스트 후 결과를 수락했습니다. 이는 이 샘플의 사용 확인이며 모든 동작의 품질이나 발 미끄러짐 제거를 보장하지 않습니다. 기준 지면 아래 발볼 관절 오차(측정 최저 약 -2.93cm)가 남아 있습니다.

추론 실패는 **Open Last Manny Transition Job** 또는 `Saved/MotionInbetweening/Inference/<작업ID>/`의 `pipeline_result.json`, `model_stderr.log`를 확인합니다. 정상 저장은 `success=true`, `stage=completed`, `asset_path`로 확인합니다. `vendor source differs` 오류는 원본 코드 변경/줄바꿈, 모델 해시 오류는 다른 버전이나 미완료 다운로드를 먼저 확인합니다.

모델 파일을 준비한 뒤 다음 검사를 실행할 수 있습니다. 공식 가중치 검사는 환경 변수를 생략하면 skipped이므로 전체 성공으로 세지 않습니다.

```powershell
$env:MIB_MODEL_CONFIG = (Resolve-Path '.\Tools\MotionInference\model_config.local.json').Path
& '.\.venv\Scripts\python.exe' -X utf8 -B -m unittest discover -s '.\Tools\MotionInference\Tests' -v
& '.\.venv\Scripts\python.exe' -X utf8 -B -m unittest discover -s '.\Plugins\MotionInbetweeningHello\Tests' -p 'test_*.py' -v
```

공식 추론 소스의 MIT 고지와 출처는 [vendor/LICENSE.txt](Tools/MotionInference/vendor/LICENSE.txt), [source_manifest.json](Tools/MotionInference/vendor/source_manifest.json)에 보존했습니다.
