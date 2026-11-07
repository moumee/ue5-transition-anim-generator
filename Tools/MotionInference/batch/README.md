# 일괄 평가·비교 도구

Manny 클립을 한 번에 내보내고, 프레임 보정을 다시 추정하고, 기존/개선 어댑터를 비교해 Unreal 에셋으로 저장하는 도구입니다. 변경 배경과 결과는 [docs/frame_refinement.md](../../../docs/frame_refinement.md)를 봅니다.

작업 폴더는 `$env:MIB_BATCH_DIR`이며, 생략하면 `Saved/MotionInbetweening/Batch`입니다. 아래 명령은 저장소 루트의 PowerShell 기준이고, `<config>`는 본인의 `Tools/MotionInference/model_config.local.json`입니다. Unreal 단계는 에디터를 닫고 실행합니다.

```powershell
$engine = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'   # 설치 위치에 맞게
$project = (Resolve-Path '.\transition_generator.uproject').Path
$py = '.\.venv\Scripts\python.exe'
$config = (Resolve-Path '.\Tools\MotionInference\model_config.local.json').Path
```

## 1. 클립 내보내기 (Unreal)

```powershell
& $engine $project -run=pythonscript "-script=$PWD\Tools\MotionInference\batch\ue_export_manny_clips.py" -unattended -nullrhi
```

Manny 스켈레톤을 쓰는 모든 Animation Sequence를 30fps 전체 길이로 `clips/<이름>/source_manny.json`에 저장합니다. 레퍼런스 스켈레톤은 `clips/skeleton.json`, 목록과 건너뛴 이유는 `clips/manifest.json`에 남습니다. 팀 추출기(`Tools/MannyLafanMapping/Unreal/extract_manny_walk_sequence.py`)를 그대로 호출합니다. Control Rig 포즈 클립과 `/Game/MotionInbetweening/` 결과는 제외합니다.

## 2. 모델 입력 만들기

```powershell
& $py -X utf8 .\Tools\MotionInference\batch\prepare_clip_inputs.py
```

각 클립에 정규화하지 않은 `model_input.json`을 만듭니다. 플러그인 내보내기와 같은 함수를 쓰며, 첫 31샘플은 플러그인 결과와 일치합니다.

## 3. 롤 재추정 (클립을 추가했을 때)

```powershell
$inputs = Get-ChildItem "$env:MIB_BATCH_DIR\clips\MM_*\model_input.json" | ForEach-Object FullName
& $py -X utf8 .\Tools\MotionInference\estimate_frame_refinement.py --inputs $inputs `
    --output .\Tools\MotionInference\configs\manny_frame_refinement_v1.json `
    --stats .\Tools\MotionInference\models\lafan1_context_model_release\train_stats_context.pkl
```

`--stats`는 보정 전후 z-score 보고용입니다. 값을 바꾼 뒤에는 `Tests/Fixtures/refined_context_detail_baseline.*`를 다시 만들고 테스트를 실행합니다.

## 4. 자기 복원 평가

```powershell
& $py -X utf8 .\Tools\MotionInference\batch\evaluate_self_inbetweening.py --config $config
```

같은 클립의 구간을 지우고 다시 채워 원본과 비교합니다. 기존 어댑터(보정·클램프 끔), 지정 설정, 단순 보간을 그룹별로 표로 출력하고 `evaluation.json`에 남깁니다. 롤은 `MM_*` 클립에서만 추정했으므로, 나머지 클립 묶음(`MF_*` Quinn, `A_*` Idle_Walk_Jog)은 검증 세트로 따로 보고합니다.

## 5. 비교 전환 생성과 에셋 저장

```powershell
& $py -X utf8 .\Tools\MotionInference\batch\make_comparison_transitions.py --config $config
& $engine $project -run=pythonscript "-script=$PWD\Tools\MotionInference\batch\ue_import_manny_poses.py" -unattended -nullrhi
```

첫 명령은 `Content/Idle_Walk_Jog`의 Idle·Walk·Jog 조합 6개를 플러그인 기본 요청(문맥 = 첫 31샘플의 마지막 10프레임, 목표 = 0번, N=20)으로 생성합니다. 다른 클립은 `--pairs MM_Walk_Fwd:MM_Run_Fwd:WalkToRun ...`처럼 `시작:목적지:이름`으로 지정합니다. 플러그인 역매핑으로 89본 포즈까지 만들고, 그럴듯함 지표를 `transitions/summary.json`에 남깁니다.

두 번째 명령은 `/Game/MotionInbetweening/Baseline`과 `/Improved`에 `AN_<전환>_N20_<버전>` 에셋을 만듭니다. 같은 이름의 이전 에셋은 이 두 폴더 안에서만 교체합니다. 저장 후 다시 평가해 JSON과의 오차를 `transitions/ue_import_report.json`에 남깁니다.

## 지표 정의 (`metrics.py`)

| 지표 | 의미 |
| --- | --- |
| 위치 오차 | FK로 구한 22관절 위치와 원본의 평균 거리(cm). Manny 레퍼런스 스켈레톤 뼈 길이 사용 |
| 경계 루트 보정량 | `infer.py`가 생성 구간 양 끝 이동량을 맞추려고 더한 수평 보정의 최대값. 모델 궤적과 목표 배치가 어긋난 정도 |
| 발 미끄러짐 | 발목·발볼이 지면 3 cm 이내에 연속으로 있는 동안의 프레임당 수평 이동(cm). 원본 클립은 약 1 |
| 속도 출렁임 | 루트 수평 속도가 0.15 cm/프레임 넘게 올랐다 내려가는(또는 반대) 횟수 |
| 최저 발끝 높이 | 원본 Walk·Run의 발볼 최저 높이 대비 생성 구간 최저값. 음수는 지면 관통 |
