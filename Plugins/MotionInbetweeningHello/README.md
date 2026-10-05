# MotionInbetweeningHello

UE 5.8 Editor 플러그인. **Manny 애니메이션을 모델 입력으로 내보내기**, **모델 출력 JSON을 Manny 애니메이션 에셋으로 가져오기**, 기존 테스트 애니메이션 생성을 제공합니다. 모델 추론 호출은 아직 연결하지 않았습니다.

## Manny 모델 입력 내보내기

1. `transition_generator.uproject`를 UE 5.8로 엽니다. 이미 열려 있었다면 새 빌드가 로드되도록 다시 엽니다.
2. **Tools → Motion In-betweening Settings**에서 Python 실행 파일과 모델의 학습 통계 파일을 지정합니다. 현재 PC에는 기존 `mib` 환경과 공식 Context 모델의 `train_stats_context.pkl`을 설정해 두었습니다.
3. Content Browser에서 Manny Skeleton의 **Animation Sequence 하나**를 선택합니다. 확인한 예제는 `Content/ControlRig/Characters/Mannequins/Animations/Manny/MM_Walk_Fwd`입니다.
4. **Tools → Export Selected Manny Model Input**을 실행합니다.
5. 성공하면 결과 폴더가 열립니다. `model_input.json`이 정렬·전처리된 모델 입력입니다.

출력 위치는 프로젝트의 `Saved/MotionInbetweening/InputMapping/<애니메이션 이름>_<고유 ID>/`입니다. 매번 새 폴더를 만들며 기본 설정은 시작부터 **30fps·최대 31샘플(1초), context 10**입니다. 10샘플보다 짧은 애니메이션은 거부합니다. 최대 샘플 수는 설정에서 10~300으로 변경할 수 있습니다.

| 파일 | 내용 |
| --- | --- |
| `job.json` | 선택 에셋, 경로, 샘플·검증 설정 |
| `source_manny.json` | Unreal에서 추출한 Manny 본 transform |
| `raw_135.json` | 팀원 코드가 변환한 원본 135차원 입력 |
| `model_input.json` | `vectors_tx135`, 관절 순서, 원점·방향 정렬 정보, 정규화 여부, 통계·소스 해시 |
| `result.json` | 성공 여부와 결과 파일 위치. 실패하면 `error.log`에 Python 오류 기록 |

Python은 **3.11 이상 + NumPy**가 필요합니다. 팀원 의존성은 프로젝트의 `Tools/MannyLafanMapping/requirements.txt`에 있습니다. Unreal 내장 Python은 애니메이션 추출에, 별도 Python은 NumPy 변환에 사용합니다. 개인 경로는 `Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini`에 저장합니다.

`Training Statistics`를 비우면 원점·방향만 정렬한 입력을 내보내며 `normalization_applied=false`로 표시합니다. 모델에 넣기 전 해당 모델의 통계로 정규화해야 합니다. 통계를 지정하면 정적 채널 처리와 정규화까지 수행합니다. 모델을 바꾸면 통계 파일도 맞춰야 합니다.

중복 실행은 막으며 **Tools → Cancel Manny Input Export**로 변환을 중지할 수 있습니다. 외부 변환은 120초 제한이고 에디터 종료 시 해당 작업을 종료합니다. 실패 이유는 `LogMotionInbetweeningHello`와 작업 폴더에 기록합니다.

입력 매핑은 [팀 PR #1](https://github.com/moumee/ue5-transition-anim-generator/pull/1)의 `c1399e3`을 그대로 호출합니다. `Tools/MannyLafanMapping` 코드는 수정하지 않았습니다. 이 메뉴는 **모델 입력 준비까지**입니다. 역매핑과 생성 에셋 저장은 아래 출력 가져오기 메뉴로 연결했으며 모델 추론 호출은 후속 작업입니다. 현재 JSON은 플러그인 연결용 형식이며 팀 전체의 최종 전달 규격으로 확정한 것은 아닙니다.

## 모델 출력 JSON을 애니메이션으로 가져오기

1. **Tools → Motion In-betweening Settings**에서 기존 Python 환경을 확인합니다.
2. **Tools → Import Model Output as Manny Animation**을 누르고 모델 출력 JSON을 선택합니다.
3. 성공하면 `Content/MotionInbetweening/Generated/AN_Manny_Output*`에 새 에셋을 저장하고 Animation Editor를 엽니다. 매번 고유 이름을 사용하므로 기존 에셋을 덮어쓰지 않습니다.
4. 처음 기능만 확인하려면 `Plugins/MotionInbetweeningHello/Tests/Fixtures/context_model_output.json`을 선택합니다. 보관된 공식 Context 모델의 실제 31프레임 출력이며, 새 추론이나 학습은 실행하지 않습니다.

출력은 **SKM_Manny_Simple의 89본**, `SK_Mannequin` Skeleton, 부모 기준 local transform, cm·정규화 XYZW quaternion으로 저장합니다. 모델이 제어하는 본은 22개이고 나머지 twist·IK·손가락 본은 reference pose입니다. `root`는 기준 포즈를 유지하고 이동은 `pelvis`에 들어갑니다. Root Motion 기능은 꺼 두었습니다. 에셋을 생성하는 성공 여부와 모션의 시각적 품질은 별도로 확인해야 합니다.

JSON은 아래 조건을 사용합니다. 이는 플러그인 연결 계약이며 팀 전체의 최종 추론 규격으로 확정한 것은 아닙니다.

| 항목 | 조건 |
| --- | --- |
| 상태 배열 | `predictions_tx135`, `output_tx135`, `vectors_tx135` 중 하나만 포함. 2~10,000행·135열·유한값 |
| 숫자 배열 타입 | 상태·시간·heading·root offset 배열은 유한한 JSON 정수·실수만 허용. 숫자 문자열·bool·null은 거부하며, 중첩·평탄 heading을 함께 주면 양쪽을 각각 검사 |
| 개수 선언 | 선택 사항인 `frame_count`, `sample_count`는 실제 행 수와 같은 정수. `shape`는 실제 `[행 수,135]`와 일치해야 함 |
| 의미 선언 | `joint_order`, `parents`가 있으면 팀 calibration의 순서·계층과 일치. `rotation_space`는 `parent_local`, `source_position_unit`는 `cm`, `layout`은 `T x (22 joints * rotation6D + Hips XYZ)`만 지원 |
| 위치 공간 선언 | `position_space`를 제공하면 `lafan_start_centered`와 heading 복원 offset 쌍이 필요. 메타데이터를 생략한 기존 최소 출력과 팀 raw root offset 경로는 유지 |
| JSON 중복 | 같은 이름의 객체 키를 중복 선언하면 최상위·중첩 객체 모두 거부 |
| 시간 | `sample_rate_hz` 명시, 0 초과 240 이하. `frame_indices`가 있으면 연속 정수. 31샘플·30fps는 양 끝을 포함해 1초 |
| 정규화 | `normalization_applied`를 bool로 명시. true면 대응하는 Training Statistics가 필요하고, false면 설정된 통계는 사용하지 않음 |
| 통계 대응 | `statistics.sha256`가 있으면 설정한 통계와 일치하는지 검사 |
| 위치·방향 복원 | 입력의 `alignment.position_offset_xz`, `alignment.rotation_offset`를 출력에도 보존. 또는 팀 역매핑의 평탄한 `heading_position_offset_xz`, `heading_rotation_offset` 쌍 사용 |
| 정렬 선언 | `heading_alignment_applied=true`인데 복원 정보가 없으면 거부. 정보가 전혀 없으면 현재 좌표의 위치·방향으로 처리 |

플러그인 입력의 `alignment`를 보존한 출력에는 `root_position_offset_lafan`을 중복으로 붙이지 않습니다. 입력 어댑터가 원래 root 위치로부터 정렬했으므로 더하면 위치가 두 번 이동합니다. 순수한 팀 raw 매핑의 root offset 방식은 별도로 지원합니다. 입력의 정규화 여부를 무조건 출력에 복사하면 안 됩니다. 추론 코드가 이미 역정규화했다면 출력은 `false`여야 합니다.

결과는 `Saved/MotionInbetweening/OutputMapping/<고유 ID>/`에 남습니다.

| 파일 | 내용 |
| --- | --- |
| `source_model_output.json` | 가져온 JSON의 원본 사본 |
| `skeleton.json` | 이번 변환에 사용한 실제 Manny Simple reference skeleton |
| `job.json` | 원본 위치, 통계·팀 도구 위치 |
| `manny_pose.json` | 팀 역매핑 결과, 89본 local pose, 원본·통계·도구 해시 |
| `result.json`, `error.log` | 외부 Python 변환 결과와 실패 이유 |
| `asset_result.json` | 실제 에셋 저장 성공 여부·에셋 경로. `result.json` 성공만으로 에셋 저장 성공을 뜻하지 않음 |

**Tools → Cancel Manny Output Import**로 외부 변환을 중지할 수 있습니다. 120초 제한과 중복 실행 방지가 있고, 에디터 종료 시 외부 작업을 종료합니다. 에셋 생성·저장은 변환이 끝난 뒤 에디터 스레드에서 수행합니다.

작업 폴더를 만든 뒤 복사·skeleton/설정 저장·Python 시작에 실패한 경우에도 쓰기 가능한 폴더에 `asset_result.json`을 남깁니다. 새 시도의 결과는 이전 성공 에셋 정보를 유지하지 않습니다. 폴더 생성 전 실패는 UI 오류와 현재 실패 상태로 확인하며, 폴더 자체에 쓸 수 없으면 영수증 저장 실패를 로그에 남깁니다.

팀 [PR #2](https://github.com/moumee/ue5-transition-anim-generator/pull/2)의 역매핑을 그대로 호출합니다. `Tools/MannyLafanMapping` 원본은 수정하지 않고 `Scripts/prepare_manny_output.py`에서 입력 메타데이터를 맞춥니다. `Private/MotionInbetweeningOutputMapping.*`는 비동기 실행·저장, `MotionInbetweeningOutputData.*`는 실제 reference skeleton 추출·JSON 검증을 담당합니다. `CreateAndSaveAnimation`에 출력 경로 인수를 추가해 기존 테스트 기능을 유지했습니다.

## 기존 테스트 애니메이션 실행

1. `transition_generator.uproject`를 UE 5.8로 엽니다. `EngineAssociation`은 `5.8`입니다.
2. 레벨 에디터의 **Tools(도구) → Motion In-betweening Plugin Test**는 기존 알림과 로그를 출력합니다.
3. **Tools → Generate Manny Test Animation**을 누르면 새 Animation Sequence를 저장하고 Animation Editor를 엽니다.
4. Manny의 척추와 왼팔이 움직이는 2초 루프를 확인합니다. 미리보기가 일시 정지되어 있다면 타임라인의 ▶를 누릅니다.
5. 다시 실행하면 별도 이름의 에셋을 만듭니다. Content Browser의 `Content/MotionInbetweening/Tests`에서 두 에셋을 확인합니다.
6. 에디터를 종료·재시작한 뒤 생성 에셋을 더블클릭해 다시 엽니다.
7. Output Log에서 `LogMotionInbetweeningHello`를 검색하면 선택한 Mesh/Skeleton, 생성 경로, 프레임 정보와 실패 이유를 볼 수 있습니다.

출력은 `/Game/MotionInbetweening/Tests/AN_Manny_Test`를 기본 이름으로 사용하며 AssetTools가 중복을 피하도록 이름을 결정합니다. 이미 존재하는 패키지는 덮어쓰지 않습니다. 저장 후 생성된 에셋만 열며 기존 Skeleton·Mesh·게임 Source는 저장하거나 수정하지 않습니다.

## 구조와 모델 연결 지점

| 파일 | 역할 |
| --- | --- |
| `Public/MotionInbetweeningHelloModule.h`, `Private/MotionInbetweeningHelloModule.cpp` | 모듈 시작·종료, Hello·생성·입력 내보내기·설정·취소 메뉴 |
| `Public/MotionInbetweeningMappingSettings.h` | 사용자별 Python·통계 경로, 샘플·검증 설정 |
| `Private/MotionInbetweeningMapping.h/.cpp` | 입력 검사, 추출, 외부 Python 실행, 결과·취소·시간 제한 처리 |
| `Scripts/export_manny_input.py`, `Scripts/prepare_manny_input.py` | 팀원 추출·변환 함수를 호출하고 결과 메타데이터를 보관하는 연결 코드 |
| `Private/MotionInbetweeningAnimationData.h` | `FAnimationSamples`, `FBoneSamples`: 본 이름과 균일 샘플의 위치·회전·스케일 |
| `Private/MotionInbetweeningTestData.cpp` | 실제 Skeleton의 reference pose로 테스트 데이터를 만들고 입력 검증 |
| `Private/MotionInbetweeningAnimSequence.h/.cpp` | Manny 로드, Animation Data Controller로 트랙 입력, 고유 이름과 패키지 저장 |
| `Private/Tests/MotionInbetweeningAnimationTests.cpp` | 잘못된 입력 거부와 저장된 에셋 재로딩·재생 데이터 검사 |
| `Private/Tests/MotionInbetweeningMappingTests.cpp` | 실제 Manny 입력 추출·변환과 잘못된 설정·중복 실행 거부 검사 |

기존 테스트 메뉴는 그대로 두고, 출력 메뉴에서 **팀 역매핑 → 검증된 FAnimationSamples → CreateAndSaveAnimation**을 호출합니다. 입력·출력 사이의 실제 모델 추론 호출과 전환 구간 선택 UI는 후속 연결 대상입니다.

현재 내부 입력 가정은 Unreal 좌표계의 **부모 기준 local 절대 transform**, 위치 **cm**, 회전 **정규화한 quaternion (X,Y,Z,W)**, 양수 스케일입니다. delta/global 포즈를 직접 넣으면 안 됩니다. 이는 데모 가정이며 팀의 합의된 전달 규격이 아닙니다. 전달하지 않은 본은 Skeleton의 reference pose를 사용합니다.

## 테스트 데이터와 에셋 생성 원리

- 실제 Mesh: `/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny.SKM_Manny`.
- Skeleton은 Mesh의 `GetSkeleton()`으로 가져옵니다. 현재 프로젝트에서는 `/Game/ControlRig/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin`입니다.
- 30fps, 샘플 61개(0~60), 시간 간격 60개, 총 2초입니다. 양 끝 샘플을 포함하므로 `SetNumberOfFrames(NumSamples - 1)`입니다.
- `root` 고정, `spine_03` local X축 ±8도, `upperarm_l` local Y축 ±35도, `lowerarm_l` local Y축 0~55도. 나머지는 reference pose입니다.
- 기준 회전에 작은 local 회전을 곱하고 정규화합니다. 위치와 스케일은 실제 기준 포즈를 유지합니다. 루트 모션은 끕니다.
- `NewObject<UAnimSequence>` → `SetSkeleton/SetPreviewMesh` → `GetController().InitializeModel()` → frame rate/length → `AddBoneCurve/SetBoneTrackKeys` → `NotifyPopulated` → 파생 애니메이션 컴파일 완료 대기 → `UPackage::SavePackage` → `AssetCreated` → Animation Editor 순서입니다.
- 데이터 변경은 scoped bracket으로 묶습니다. 생성·디스크 저장 전체에 대한 Undo는 제공하지 않습니다.
- 입력은 본 중복·누락, 키 개수, 유한값, 회전 정규화, 양수 스케일, 프레임률을 검사합니다. 실패 시 전용 로그와 알림으로 이유를 표시합니다.

`Build.cs`는 기존 의존성에 ContentBrowser, DeveloperSettings, Settings, Projects, Json, PythonScriptPlugin을 추가합니다. `.uplugin`은 Editor/Default 모듈을 유지하며 PythonScriptPlugin과 EditorScriptingUtilities를 사용합니다. NumPy는 별도 Python 환경에 설치합니다.

## 빌드

에디터를 닫고 PowerShell에서 실행합니다.

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' transition_generatorEditor Win64 Development 'C:\Users\young\prg\UnrealEngine\ue5-transition-anim-generator\transition_generator.uproject' -WaitMutex -NoHotReloadFromIDE
```

## 자동화 검증

`MotionInbetweening.InputValidation`은 실제 Skeleton으로 데이터 생성 및 잘못된 입력 거부를 검사합니다. `MotionInbetweening.SavedAssets`는 메뉴로 **최소 두 번 생성한 뒤** 실행하는 재로딩 검사입니다. 테스트 자체는 에셋을 생성하지 않습니다. 전용 출력 폴더의 `AN_Manny_Test*`를 검사하므로 의도적으로 편집한 예전 데모가 있다면 별도로 보관하고 검사합니다.

`MotionInbetweening.MannyInputMapping`은 설정한 Python으로 실제 `MM_Walk_Fwd`를 내보내고 31×135 유한값·30fps·context 10·정규화 여부를 검사합니다. Python 설정이 필요하며 결과는 일반 내보내기와 같은 `Saved` 경로에 남습니다. 이 검사는 새 메뉴와 같은 C++ 실행 경로를 사용합니다.

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'C:\Users\young\prg\UnrealEngine\ue5-transition-anim-generator\transition_generator.uproject' -Unattended -NoSplash -NoSound -NullRHI '-ExecCmds=Automation RunTests MotionInbetweening.' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=C:\Users\young\prg\UnrealEngine\ue5-transition-anim-generator\Saved\Automation\MotionInbetweening' '-AbsLog=C:\Users\young\prg\UnrealEngine\ue5-transition-anim-generator\Saved\Logs\MotionInbetweening-Verify.log'
```

저장 검사는 Skeleton/preview mesh, 30fps·60구간·61키·2초, 모든 입력 키, 루프 양 끝, 압축 데이터로 평가한 팔 움직임을 확인합니다. UI 재생 검증은 별도로 수행합니다. `Binaries`, `Intermediate`, `Saved`는 기존 `.gitignore`로 제외합니다.

2026-09-30 검증: Editor 빌드 성공, 위 Unreal 자동화 3개 성공(경고·실패 0), 기존 저장 애니메이션 5개 검사, 팀원 단위 테스트 10개 성공. 별도로 정규화 사용·미사용과 잘못된 샘플 시간 처리 3건을 확인했습니다. 새 메뉴 클릭·폴더 열기와 시각적인 모션 품질은 이번 자동화 검사 범위에 포함하지 않았습니다.

작업 기록: `C:/Users/young/prg/School/게임 공학/지식/20_Manny입력매핑_플러그인연결_2026-09-30.md`. 기존 애니메이션 생성 기록은 같은 폴더의 `09_Manny테스트애니메이션_구현검증_2026-09-14.md`에 있습니다.

## 출력 기능 자동화 검사

`MotionInbetweening.MannyOutputMapping`은 실제 Context 출력 fixture를 두 번 가져와 별도 에셋 저장, 89본·31샘플·30fps·1초, 모든 local 키와 재생 값을 검사합니다. 정규화한 quaternion의 상대 회전으로 오차를 계산합니다. raw 재생 기준은 위치 0.001cm·회전 0.001도이고, 기본 ACL 압축의 정밀도를 참고한 압축 재생 기준은 위치 0.01cm·회전 0.2도입니다.

Windows에서는 성공한 가져오기 뒤에 실제 JSON 파일을 실행 파일로 지정해 프로세스 시작 실패를 유도하고, 실패 영수증과 이전 성공 상태 초기화를 검사합니다. 이 단계의 Windows `CreateProc failed`/`URL` 경고 2개만 예상 로그로 지정합니다.

Python 입력 검증 회귀 검사는 프로젝트 루트에서 설정한 Python으로 실행합니다. NumPy 외에 새 패키지는 필요하지 않으며, 팀 매핑이나 모델을 실행하지 않습니다.

```powershell
& 'C:\Users\young\miniconda3\envs\mib\python.exe' -B 'Plugins/MotionInbetweeningHello/Tests/test_prepare_manny_output.py' -v
```

관절 순서·단위/공간·계층, 선언 count/shape, 중복 키, 숫자 배열의 문자열·bool·null 거부와 중첩·평탄 heading 동시 검사를 수행합니다. 기존 최소 출력·정상 메타데이터·정수와 실수 혼합·UTF-8 BOM 호환성도 검사합니다. 다른 PC에서는 위 Python 경로를 해당 환경으로 바꿉니다.

그다음 새 에디터 프로세스에서 `MotionInbetweeningReload.OutputAssets`를 실행하면 성공한 출력 작업의 저장 에셋을 다시 불러와 같은 검사를 합니다. 의도적으로 생성 에셋을 편집했다면 원래 변환 결과와 달라져 재로딩 검사가 실패할 수 있습니다.

파일 선택 창·메뉴 클릭·미리보기 자동 열기와 모션의 시각적인 자연스러움은 자동화 검사의 범위 밖입니다. 이번 작업의 빌드·실행·오류 기록은 `C:/Users/young/prg/School/게임 공학/구현/2026-10-05_Manny출력연결`에 보관합니다.

2026-10-05 검증: UE 5.8.3 최종 빌드 성공, 기존 3개 검사·출력 가져오기·새 프로세스 재로딩 통과, Python 어댑터 13건 통과. 같은 Context 출력으로 생성한 6개 에셋을 재로딩했습니다. 압축 재생 최대 오차는 약 0.003616cm·0.117106도이며 원본 키는 변환 결과와 일치합니다. 초기 빌드 및 압축 검사 기준의 실패·수정 기록도 보존했습니다. 모델 추론 호출은 아직 연결하지 않았습니다.
