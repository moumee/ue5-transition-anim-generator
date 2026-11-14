# Manny Motion In-betweening

Unreal Engine 5.8의 **Motion Transition Editor**에서 여러 Manny 동작을 그래프에 끌어 놓고, 방향별 전환 프레임을 지정하면 중간 Animation Sequence와 **게임에서 사용할 Animation Blueprint(ABP)**까지 생성합니다. 동작 추가·자동 입력 변환·전환 생성·환경 설정·미리보기를 한 창에서 처리합니다. Windows · UE 5.8.3 · Python 3.11에서 검증했습니다. 생성 모델은 CPU에서 실행하며 새 학습은 필요 없습니다.

현재 대상은 Manny의 제자리 반복 동작입니다. 생성 ABP를 캐릭터에 지정하고 게임 로직에서 동작 이름을 요청하면, 현재 루프 끝 → 생성된 전환 → 다음 동작 순서로 재생합니다. 캐릭터 이동과 입력·속도에 따른 동작 선택은 게임 쪽에서 연결합니다.

## 저장소 가져오기와 빌드

Git LFS, Unreal Engine 5.8, Unreal C++ 빌드 도구가 필요합니다. 저장소를 받을 폴더에서 PowerShell로 실행합니다. 빌드용 저장소는 `C:/work`처럼 짧은 영문 경로에 두세요. 게시 전 새 빌드 검사에서 한글이 포함된 검증 경로는 MSVC C1083(PCH 파일 생성 실패)이 발생했고, 영문 경로에서는 빌드가 통과했습니다. 입력·모델 파일의 한글/공백 경로 지원과 C++ 프로젝트 빌드 경로는 별개입니다.

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

### 프레임 보정 전후 비교

`Content/MotionInbetweening/Baseline`과 `Content/MotionInbetweening/Improved`에 `Content/Idle_Walk_Jog` 클립(A_Idle·A_WalkFwd_Loop·A_JogFwd_Loop) 사이 전환 6개(N=20)의 보정 전·후 결과가 있습니다. `AN_Manny_Output23`은 보정 전에 만든 기존 데모입니다. 현재 `infer.py`는 Manny 팔 체인 롤 보정, 아래팔 비틀림 이동, 쇄골·팔 입력 클램프를 기본으로 적용합니다. 원인·변경·검증은 [docs/frame_refinement.md](docs/frame_refinement.md)를 봅니다.

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

성공하면 `Saved/ModelSmoke/model_output.json`을 생성하고 종료 코드 0을 반환합니다. 이 명령은 환경 확인용입니다. 실제 동작 선택과 Unreal 에셋·ABP 생성은 아래 통합 창에서 진행합니다.

### Unreal 설정과 생성

**도구 → Motion Transition Editor**를 열고 오른쪽 **환경 설정**에서 아래 파일을 현재 PC의 절대 경로로 선택합니다. `<저장소>`는 이 프로젝트를 받은 위치입니다. 이전 개별 생성·입력·설정 메뉴는 이 창으로 통합했습니다.

| 설정 | 값 |
| --- | --- |
| Python Executable | `<저장소>/.venv/Scripts/python.exe` |
| Inference Python | 같은 Python 또는 빈 값(위 Python 사용) |
| Inference Script | `<저장소>/Tools/MotionInference/infer.py` |
| Model Config | `<저장소>/Tools/MotionInference/model_config.local.json` |
| Training Statistics | 첫 실행은 빈 값. 비정규화 입력을 모델 진입점이 정규화함 |
| Transition Samples | `15` — 새 연결의 기본값. 각 연결의 길이는 그래프에서 변경 |
| Maximum Root Step (cm per sample) | `20` — Validation 고급 항목. Run 직접 내보내기에 필요 |

개인 설정은 `Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini`에 저장합니다. 다른 사람의 절대 경로 설정이나 이 프로젝트 외부의 준비 스크립트는 필요 없습니다.

1. 왼쪽 목록에서 `MM_Idle`, `MM_Walk_Fwd`, `MM_Run_Fwd`를 Ctrl/Shift로 선택해 **그래프로 끌어 놓습니다**. 노드가 놓은 위치에 추가되고 입력을 자동 변환합니다. 콘텐츠 브라우저에서도 직접 드래그할 수 있습니다. 중복 동작은 건너뜁니다.
2. 각 노드가 **Ready**가 되면 **오른쪽 점 → 목적 노드**를 차례로 클릭해 연결합니다. 연결은 단방향이며, **모든 방향 연결**로 왕복 연결을 한꺼번에 만들 수도 있습니다.
3. 연결선의 숫자를 선택하고 오른쪽 **생성 프레임 수**를 입력합니다. 공식 모델은 5~45프레임, 유효한 긴 전환 모델을 별도로 설정하면 최대60프레임입니다. N은 두 앵커 사이의 생성 샘플 수이며 실제 전환 길이는 `(N+1)/30`초입니다.
4. **생성 + ABP**를 누릅니다. 변경된 입력과 연결을 처리하고, 모든 연결이 성공하면 `ABP_Transitions…`를 저장합니다. 기본 동작에서 모든 동작에 도달하도록 그래프를 연결해야 합니다. 원본·이전 결과는 유지하고 새 버전으로 저장합니다.
5. 아래 동작 이름 버튼으로 ABP의 전환을 확인합니다. 입력 변환 실패 시에만 **변환 실패 재시도**가 나타납니다. 전환 생성 실패는 연결을 선택해 원인을 수정한 뒤 **생성 + ABP**로 재시도합니다. 정상 결과는 재사용합니다.

생성 결과는 `Content/MotionInbetweening/Sets/Set_<ID>/`, 편집 세트는 `Saved/MotionInbetweening/TransitionSets/`에 저장됩니다. **세트 열기**에서 저장된 그래프를 다시 열 수 있습니다. 입력 원본은 `Content/ControlRig/Characters/Mannequins/Animations/Manny`에서 찾을 수 있습니다. 전체 설정과 제한은 [플러그인 안내](Plugins/MotionInbetweeningHello/README.md), 모델 규약은 [추론 안내](Tools/MotionInference/README.md)를 참고합니다.

### 생성한 ABP를 캐릭터에 적용하기

1. 같은 Manny Skeleton을 사용하는 캐릭터의 Skeletal Mesh에서 **Animation Mode = Use Animation Blueprint**, **Anim Class = 생성된 ABP_Transitions…**를 지정합니다.
2. 캐릭터 Blueprint에서 `Get Mesh → Get Anim Instance → Cast to MotionTransitionAnimInstance → Request Motion`을 연결합니다. 요청 이름은 그래프에 표시된 동작 이름입니다. 예를 들어 `MM_Idle`, `MM_Walk_Fwd`, `MM_Run_Fwd`를 전달합니다.
3. 정지·걷기·달리기를 선택하는 입력/속도 조건과 캐릭터 이동은 게임 로직에서 연결합니다. 생성 ABP에는 동작·전환 에셋과 기본 동작이 설정되어 있으므로 전환 재생 로직을 다시 만들 필요는 없습니다.

요청은 현재 루프 끝에서 실행되며 전환 중 새 요청은 마지막 것 하나를 대기시킵니다. 직접 연결이 없는 요청은 현재 동작을 유지합니다. 생성 ABP는 제자리 포즈 재생용으로 Root Motion·Anim Notify·곡선을 복사하지 않습니다. 실행 중에는 Python이나 모델이 필요 없고, 플러그인의 **MotionTransitionRuntime** 모듈과 ABP가 참조하는 동작·전환 에셋이 필요합니다.

팀에 생성 결과를 공유할 때는 해당 세트의 `.uasset`을 Git LFS로 함께 관리합니다. 편집을 이어가려면 Git 제외 경로인 `Saved`의 세트 JSON도 별도로 보관해야 합니다. JSON만으로 런타임 ABP가 대체되지는 않습니다.

## 확인 범위와 문제 확인

2026-11-14 통합 흐름에서 3동작 자동 변환·6방향 실제 추론·게임용 ABP 생성, 생성 직후와 새 프로세스의 실제 출력 포즈, 취소/재시도·변경 연결만 재생성, Win64 게임 타깃 빌드와 생성 세트/의존성의 최소 쿠킹을 확인했습니다. 다중 드래그·축소 상태 배치·중복 방지와 정상 상태의 버튼 숨김도 UI에서 확인했습니다. 전체 프로젝트 패키징·캐릭터 이동 연동·모든 동작의 접지 품질까지 검증했다는 뜻은 아닙니다.

새 Python 환경 설치, 모델/좌표 16개와 호출기 27개 검사, UE 빌드·실제 추론/에셋 저장·새 프로세스 재로드·전체 메뉴 흐름을 확인했습니다. 2026-10-31 사용자가 Walk→Run 테스트 후 결과를 수락했습니다. 이는 이 샘플의 사용 확인이며 모든 동작의 품질이나 발 미끄러짐 제거를 보장하지 않습니다. 기준 지면 아래 발볼 관절 오차(측정 최저 약 -2.93cm)가 남아 있습니다.

2026-11-07 프레임 보정·아래팔 비틀림 이동 변경 후 README 고정 환경(Python 3.11.9 · torch 2.11.0 · numpy 1.26.4)과 공식 가중치로 모델/좌표 29개, 호출기 27개, 매핑 15개 검사가 통과했습니다. 비교 에셋 12개는 UE 5.8.3에서 생성 후 새 프로세스로 다시 열어 확인했습니다. 발 IK가 없으므로 일부 전환에는 지면 관통(Idle_Walk_Jog에서 최대 약 -4.7cm)이 남습니다.

추론 실패는 통합 창에서 해당 연결을 선택해 **실행 로그 폴더**를 열거나 `Saved/MotionInbetweening/Inference/<작업ID>/`의 `pipeline_result.json`, `model_stderr.log`를 확인합니다. 개별 추론 결과의 정상 저장은 `success=true`, `stage=completed`, `asset_path`로 확인합니다. 모든 연결이 성공해야 최종 ABP가 생성됩니다. `vendor source differs` 오류는 원본 코드 변경/줄바꿈, 모델 해시 오류는 다른 버전이나 미완료 다운로드를 먼저 확인합니다.

모델 파일을 준비한 뒤 다음 검사를 실행할 수 있습니다. 공식 가중치 검사는 환경 변수를 생략하면 skipped이므로 전체 성공으로 세지 않습니다.

```powershell
$env:MIB_MODEL_CONFIG = (Resolve-Path '.\Tools\MotionInference\model_config.local.json').Path
& '.\.venv\Scripts\python.exe' -X utf8 -B -m unittest discover -s '.\Tools\MotionInference\Tests' -v
& '.\.venv\Scripts\python.exe' -X utf8 -B -m unittest discover -s '.\Plugins\MotionInbetweeningHello\Tests' -p 'test_*.py' -v
```

공식 추론 소스의 MIT 고지와 출처는 [vendor/LICENSE.txt](Tools/MotionInference/vendor/LICENSE.txt), [source_manifest.json](Tools/MotionInference/vendor/source_manifest.json)에 보존했습니다.
