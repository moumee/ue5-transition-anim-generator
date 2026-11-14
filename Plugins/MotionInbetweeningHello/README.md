# Motion Transition Editor

UE 5.8에서 동작 선택부터 입력 변환, 방향별 전환 생성, ABP 구성과 재생 확인까지 한 창에서 처리합니다. **Tools → Motion Transition Editor**를 여세요. 기존 개별 입력·생성·가져오기·진단 메뉴와 별도 Project Settings 항목은 숨겼습니다. 매핑·추론 기능과 필요한 Python 플러그인은 내부에서 계속 사용합니다.

## 빠른 사용법

1. 왼쪽 목록에서 `MM_Idle`, `MM_Walk_Fwd`, `MM_Run_Fwd` 등 **Manny Animation Sequence**를 Ctrl/Shift로 여러 개 선택하고 **그래프로 끌어 놓습니다**. 놓은 위치에 노드가 추가되고 제자리 반복 재생 사본과 모델 입력이 자동으로 만들어집니다. 콘텐츠 브라우저에서도 직접 끌어 놓을 수 있으며, 이미 추가한 동작은 중복으로 넣지 않습니다.
2. 그래프 노드의 **오른쪽 점 → 목적 동작**을 차례로 클릭합니다. 연결은 단방향입니다. 돌아오는 전환도 필요하면 반대 방향을 연결하거나 **모든 방향 연결**을 사용합니다.
3. 선 위의 프레임 숫자를 눌러 오른쪽에서 **생성 프레임 수**를 입력하고 Enter를 누릅니다. 노드를 끌어 배치하고, 휠로 확대·축소하며, 오른쪽 버튼 드래그로 화면을 이동합니다.
4. **생성 + ABP**를 누릅니다. 입력이 바뀌었으면 다시 변환하고, 필요한 연결만 순서대로 추론합니다. 모든 연결이 성공하면 ABP가 자동 생성되어 아래 미리보기에서 재생됩니다.
5. 아래 동작 이름 버튼을 눌러 전환을 확인합니다. **이 연결 재생**은 출발 끝부분·전환·도착을 연속으로 보여 줍니다. 다시 동작 요청을 시험할 때는 **ABP 미리보기**를 누릅니다.

오른쪽 **환경 설정**에서 Python, 추론 스크립트, 모델 설정 JSON, 통계, 시간 제한을 변경합니다. 환경 설치·모델 파일 준비는 [추론 안내](../../Tools/MotionInference/README.md)를 따릅니다. 실행 중 설정과 그래프 변경은 잠기며 **취소**로 현재 작업을 중지할 수 있습니다. 성공한 결과는 남고 다음 생성에서 미완료 연결을 재시도합니다. 실패한 노드·연결을 선택하면 이유와 실행 로그를 확인할 수 있습니다. **변환 실패 재시도**는 동작의 입력 변환에 실패했을 때만 왼쪽에 나타납니다. 원인을 수정한 뒤 누르면 정상 결과는 재사용하고 필요한 입력을 다시 변환합니다. 재시도 중과 실패가 해소된 뒤에는 버튼이 숨겨집니다.

세트 이름·노드·연결·프레임·에셋 참조는 자동 저장됩니다. **세트 열기**에서 이름으로 다시 열 수 있고 다음 실행에서는 마지막 세트를 복원합니다. `Saved/MotionInbetweening/TransitionSets/`의 JSON과 `Content/MotionInbetweening/Sets/`의 에셋을 함께 보관하세요. 세트에서 항목을 제외해도 기존 에셋은 지우지 않습니다.

## 생성·재생 규칙

- 현재 대상은 **Manny Simple 89본과 같은 Skeleton의 비가산 반복 동작**, 30fps 기준 11~300샘플입니다. 다른 스켈레톤·가산 애니메이션·너무 짧거나 긴 클립은 이유를 표시하고 거부합니다.
- 생성용 사본은 root 이동·yaw와 pelvis XY 이동을 제거한 **제자리 애니메이션**입니다. 끝 0.1초를 첫 포즈에 연결해 루프를 닫습니다. 원본 애니메이션은 변경하지 않습니다.
- 입력 프레임 수 N은 두 앵커 **사이에 생성하는 샘플 수**입니다. 재생 전환은 N+2키, 길이는 `(N+1)/30`초입니다. 원시 모델 결과는 문맥 10 + N + 목표 1키로 별도 유지합니다.
- 현재 공식 모델 설정은 N=5~45입니다. PR #6의 올바른 긴 전환 모델 설정·가중치를 연결하면 최대 60을 허용하며 실행 시 백엔드가 모델·해시를 검증합니다. 긴 모델 없는 상태에서 60을 생성했다고 가정하지 않습니다.
- 모든 89본의 전환 시작·끝을 준비한 루프에 맞춰 위치·회전 잔차를 완만하게 보정합니다. 경계 일치는 발 접지·미끄러짐과 전체 중간 포즈 품질을 보장하지 않습니다.
- ABP는 요청한 동작으로 바로 점프하지 않고 **현재 루프 끝 → 전환 → 도착 루프** 순서로 재생합니다. 전환 중 요청은 현재 전환 완료 후 처리하며 남은 tick 시간은 다음 클립으로 넘깁니다. 직접 연결이 없으면 현재 동작을 유지합니다.
- 생성 ABP는 제자리 포즈 재생용이며 **Root Motion, Anim Notify, 곡선·커브 복사**는 지원하지 않습니다. 이동은 캐릭터 이동 로직에서 처리합니다. 임의 위상에서 즉시 전환·경로 자동 탐색·Undo/Redo는 이번 구현 범위에 없습니다.

## 게임에서 사용

같은 Manny Skeleton을 사용하는 캐릭터 Skeletal Mesh의 **Animation Mode = Use Animation Blueprint**, **Anim Class = 생성된 ABP_Transitions…**로 지정합니다. 캐릭터 Blueprint에서 `Get Mesh → Get Anim Instance → Cast to MotionTransitionAnimInstance → Request Motion`에 그래프의 동작 이름을 전달하거나 `RequestedMotion`을 설정합니다. 기본 동작은 노드를 선택해 **기본 동작으로 지정**합니다. 키 입력·속도 조건에 따른 이름 선택과 캐릭터 이동은 게임 로직에서 연결합니다. ABP에 저장된 동작·전환과 재생 스케줄러는 그대로 사용할 수 있습니다.

ABP에는 동작과 방향별 전환 에셋 참조가 저장됩니다. 실행 중 Python이나 모델 추론은 하지 않습니다. 플러그인의 **MotionTransitionRuntime 모듈은 게임에서도 활성화**되어야 합니다. 현재 ABP는 일반 상태 머신 노드를 펼치는 방식이 아니라, 네이티브 스케줄러와 Sequence Evaluator로 연결 규칙·정확한 재생 시간을 적용합니다.

팀에 결과를 공유할 때는 생성 세트의 `.uasset`과 이 플러그인을 함께 관리합니다. 편집 세트 JSON은 Git에서 제외되는 `Saved` 아래에 있으므로 별도 보관합니다. 다른 PC의 입력·모델 경로와 해시가 달라지면 편집 세트에서 다시 변환할 수 있으며, 이미 생성한 런타임 ABP 재생은 Python이나 원래 PC의 절대 경로에 의존하지 않습니다.

## 개발 검증

`MotionInbetweeningUX.Integrated.Create`는 실제 3개 동작 자동 변환·6방향 추론·ABP 즉시 재생·취소/재시도·한 연결 수정·저장을 검사합니다. `Integrated.Reload`는 새 프로세스에서 저장 결과와 실제 출력 포즈를 대조합니다. `Integrated.Blueprint`는 준비된 세트로 ABP 생성 직후 재생을 별도 검사합니다. 각 테스트는 프로젝트 로컬 모델 환경과 예제 에셋이 필요합니다.

아래는 기존 내부 명령·데이터 규약의 참고 기록입니다. 예전 Tools 메뉴를 다시 찾을 필요는 없습니다.


<details>
<summary>이전 메뉴·데이터 규약 참고 (개별 메뉴는 숨김)</summary>

# MotionInbetweeningHello

UE 5.8 Editor 플러그인. **Manny 입력 내보내기**, **외부 추론 실행과 결과 에셋 저장**, **출력 JSON 직접 가져오기**, 기존 테스트 애니메이션 생성을 제공합니다. 공식 Context + Detail 모델의 CPU 진입점과 Walk·Run 입력 샘플은 [Tools/MotionInference](../../Tools/MotionInference/README.md)에 있습니다. 모델 파일과 실행 환경을 먼저 준비합니다.

처음 받은 PC에서는 [저장소 설치·데모·생성 안내](../../README.md)를 먼저 따릅니다.

## Manny 모델 입력 내보내기

1. `transition_generator.uproject`를 UE 5.8로 엽니다. 이미 열려 있었다면 새 빌드가 로드되도록 다시 엽니다.
2. **Tools → Motion In-betweening Settings**에서 Python 실행 파일과 모델의 학습 통계 파일을 지정합니다. 각 PC의 Python을 지정합니다. 첫 실행은 Training Statistics를 비워 raw 입력으로 시작할 수 있으며, 정규화 입력은 공식 Context 모델의 `train_stats_context.pkl`을 지정합니다.
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

입력 매핑은 [팀 PR #1](https://github.com/moumee/ue5-transition-anim-generator/pull/1)의 `c1399e3`을 그대로 호출합니다. `Tools/MannyLafanMapping` 코드는 수정하지 않았습니다. 이 메뉴는 **모델 입력 준비까지**입니다. 준비한 파일은 아래 추론 메뉴 또는 팀 추론 코드에서 사용합니다. 현재 JSON은 플러그인 연결용 형식이며 팀 전체의 최종 전달 규격으로 확정한 것은 아닙니다.

## 시작·도착 입력으로 전환 생성

[실제 모델 준비와 처리 규약](../../Tools/MotionInference/README.md)에 따라 `infer.py`와 개인 모델 설정을 준비한 뒤 다음 순서로 사용합니다. 현재 어댑터는 5~45 전환 샘플을 지원합니다. 정규화한 옛 입력에 투영 전 발가락 회전이 없으면 새 플러그인으로 다시 내보냅니다. `MM_Run_Fwd`는 입력 검사의 `Max Root Step Cm`을 20 이상으로 설정해야 합니다.

1. 시작·도착 Manny 애니메이션을 각각 위 메뉴로 내보내 `model_input.json` 두 개를 준비합니다. 같은 모델 통계 설정을 사용합니다.
2. **Tools → Motion In-betweening Settings**에서 `Inference Script`, `Model Config`를 지정합니다. 모델용 Python 환경이 다르면 `Inference Python`도 지정합니다. 비우면 기존 `Python Executable`을 사용합니다.
3. `Transition Samples`는 마지막 문맥과 도착 목표 사이의 샘플 수입니다. 기본 20이면 문맥 10 + 생성 20 + 목표 1 = 31샘플입니다. 모델의 지원 길이를 확인해 설정합니다.
4. **Tools → Generate Manny Transition from Model Inputs**를 누르고 START 입력, DESTINATION 입력 순서로 선택합니다. 시작 파일의 마지막 10샘플과 도착 파일의 첫 샘플을 사용합니다.
5. 추론·출력 검증·역매핑·저장이 성공하면 새 Manny 에셋을 열어 줍니다. 실제 생성 품질은 에디터에서 따로 확인합니다.

**Cancel Manny Transition Generation**으로 중지하고 **Open Last Manny Transition Job**으로 실행 기록을 엽니다. 기본 제한은 300초이며 설정할 수 있습니다. 취소·시간 초과·에디터 종료는 호출기와 자식 추론 프로세스를 함께 종료합니다. 결과 역매핑 단계에는 기존 120초 제한이 적용됩니다. 추론 중에는 다른 추론이나 직접 출력 가져오기를 시작할 수 없습니다.

작업별 `Saved/MotionInbetweening/Inference/<ID>/`에 입력 사본, `job.json`, `request.json`, `model_stdout.log`, `model_stderr.log`, `model_output.json`, Python 검증 `result.json`과 최종 `pipeline_result.json`을 보관합니다. 실패 위치에 따라 일부 파일은 없을 수 있으며 쓰기 가능한 작업 폴더에는 최종 실패·취소 상태를 남깁니다. `pipeline_result.json`의 `success=true`와 `stage=completed`가 실제 에셋 저장 완료입니다. 출력 변환 폴더와 에셋 경로도 이 파일에서 찾습니다.

이 버전은 내보낸 구간을 사용하며 애니메이션의 임의 시간 구간을 지정하는 UI는 아직 없습니다. 시험용 `Tests/Fixtures/inference_fixture_backend.py`는 저장된 포즈를 반복할 뿐이며 실제 추론으로 사용하지 않습니다.

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

기존 테스트 메뉴는 그대로 두고, 출력 메뉴에서 **팀 역매핑 → 검증된 FAnimationSamples → CreateAndSaveAnimation**을 호출합니다. `Private/MotionInbetweeningInference.*`와 `Scripts/run_manny_inference.py`는 외부 추론과 이 출력 경로를 연결합니다. `Tools/MotionInference/infer.py`가 공식 모델 추론을 수행하며, 애니메이션 임의 시간 구간 선택 UI는 후속 대상입니다.

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

에디터를 닫고 저장소 루트의 PowerShell에서 실행합니다.

```powershell
$projectFile = (Resolve-Path '.\transition_generator.uproject').Path
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' transition_generatorEditor Win64 Development $projectFile -WaitMutex -NoHotReloadFromIDE
```

## 자동화 검증

`MotionInbetweening.InputValidation`은 실제 Skeleton으로 데이터 생성 및 잘못된 입력 거부를 검사합니다. `MotionInbetweening.SavedAssets`는 메뉴로 **최소 두 번 생성한 뒤** 실행하는 재로딩 검사입니다. 테스트 자체는 에셋을 생성하지 않습니다. 전용 출력 폴더의 `AN_Manny_Test*`를 검사하므로 의도적으로 편집한 예전 데모가 있다면 별도로 보관하고 검사합니다.

`MotionInbetweening.MannyInputMapping`은 설정한 Python으로 실제 `MM_Walk_Fwd`를 내보내고 31×135 유한값·30fps·context 10·정규화 여부를 검사합니다. Python 설정이 필요하며 결과는 일반 내보내기와 같은 `Saved` 경로에 남습니다. 이 검사는 새 메뉴와 같은 C++ 실행 경로를 사용합니다.

```powershell
$projectFile = (Resolve-Path '.\transition_generator.uproject').Path
$reportPath = Join-Path (Get-Location) 'Saved/Automation/MotionInbetweening'
$logPath = Join-Path (Get-Location) 'Saved/Logs/MotionInbetweening-Verify.log'
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' $projectFile -Unattended -NoSplash -NoSound -NullRHI '-ExecCmds=Automation RunTests MotionInbetweening.' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$reportPath" "-AbsLog=$logPath"
```

저장 검사는 Skeleton/preview mesh, 30fps·60구간·61키·2초, 모든 입력 키, 루프 양 끝, 압축 데이터로 평가한 팔 움직임을 확인합니다. UI 재생 검증은 별도로 수행합니다. `Binaries`, `Intermediate`, `Saved`는 기존 `.gitignore`로 제외합니다.

2026-09-30 검증: Editor 빌드 성공, 위 Unreal 자동화 3개 성공(경고·실패 0), 기존 저장 애니메이션 5개 검사, 팀원 단위 테스트 10개 성공. 별도로 정규화 사용·미사용과 잘못된 샘플 시간 처리 3건을 확인했습니다. 새 메뉴 클릭·폴더 열기와 시각적인 모션 품질은 이번 자동화 검사 범위에 포함하지 않았습니다.


## 출력 기능 자동화 검사

추론 호출 검사는 프로젝트 루트에서 Python으로 `-X utf8 -B -m unittest discover -s Plugins/MotionInbetweeningHello/Tests -p "test_*.py" -v`를 실행합니다. 공백·한글 경로의 실제 자식 프로세스, 입력·요청 ID·출력 길이·정규화 통계·heading 검증, 오류 로그 보존을 포함합니다.

`MotionInbetweening.MannyInference`는 시험용 실행기로 호출→출력 변환→에셋 저장, 중복 실행 방지, 실패 상태 초기화, 자식 프로세스를 포함한 취소·시간 초과를 검사합니다. 실제 모델을 실행하거나 모션 품질을 평가하지 않습니다. 성공한 에셋은 아래 새 프로세스 재로딩 검사에도 포함됩니다.

`MotionInbetweeningOptional.RealModel`은 별도로 선택해 실행하는 실제 모델 통합 검사입니다. `-MIBInferencePython=<Python 절대 경로>`와 `-MIBModelConfig=<모델 설정 절대 경로>`를 함께 전달해야 하며, 누락하면 실패합니다. 공식 Context + Detail의 Walk→Run 출력이 호출기·역매핑을 거쳐 31키·89본 에셋으로 저장되는지 확인합니다. 전체 `MotionInbetweening.` 검사에 자동 포함되지 않으며 시각 품질을 승인하지 않습니다.

`MotionInbetweening.MannyOutputMapping`은 실제 Context 출력 fixture를 두 번 가져와 별도 에셋 저장, 89본·31샘플·30fps·1초, 모든 local 키와 재생 값을 검사합니다. 정규화한 quaternion의 상대 회전으로 오차를 계산합니다. raw 재생 기준은 위치 0.001cm·회전 0.001도이고, 기본 ACL 압축의 정밀도를 참고한 압축 재생 기준은 위치 0.01cm·회전 0.2도입니다.

Windows에서는 성공한 가져오기 뒤에 실제 JSON 파일을 실행 파일로 지정해 프로세스 시작 실패를 유도하고, 실패 영수증과 이전 성공 상태 초기화를 검사합니다. 이 단계의 Windows `CreateProc failed`/`URL` 경고 2개만 예상 로그로 지정합니다.

Python 입력 검증 회귀 검사는 프로젝트 루트에서 설정한 Python으로 실행합니다. NumPy 외에 새 패키지는 필요하지 않으며, 팀 매핑이나 모델을 실행하지 않습니다.

```powershell
& '.\.venv\Scripts\python.exe' -B 'Plugins/MotionInbetweeningHello/Tests/test_prepare_manny_output.py' -v
```

관절 순서·단위/공간·계층, 선언 count/shape, 중복 키, 숫자 배열의 문자열·bool·null 거부와 중첩·평탄 heading 동시 검사를 수행합니다. 기존 최소 출력·정상 메타데이터·정수와 실수 혼합·UTF-8 BOM 호환성도 검사합니다. 다른 PC에서는 위 Python 경로를 해당 환경으로 바꿉니다.

그다음 새 에디터 프로세스에서 `MotionInbetweeningReload.OutputAssets`를 실행하면 성공한 출력 작업의 저장 에셋을 다시 불러와 같은 검사를 합니다. 의도적으로 생성 에셋을 편집했다면 원래 변환 결과와 달라져 재로딩 검사가 실패할 수 있습니다.

파일 선택 창·메뉴 클릭·미리보기 자동 열기와 모션의 시각적인 자연스러움은 자동화 검사의 범위 밖입니다. 자신의 실행 기록은 프로젝트의 `Saved` 작업 폴더에서 확인합니다.

2026-10-05 검증: UE 5.8.3 최종 빌드 성공, 기존 3개 검사·출력 가져오기·새 프로세스 재로딩 통과, Python 어댑터 13건 통과. 같은 Context 출력으로 생성한 6개 에셋을 재로딩했습니다. 압축 재생 최대 오차는 약 0.003616cm·0.117106도이며 원본 키는 변환 결과와 일치합니다. 초기 빌드 및 압축 검사 기준의 실패·수정 기록도 보존했습니다. 모델 추론 호출은 아직 연결하지 않았습니다.

## 전환 재생 경계 검사 — UX 브랜치 첫 구현

`Tools → Motion Transition Playback Check`에서 원본 Source·Destination과 이미 생성한 결과를 선택합니다. 생성 당시의 N, 출발 시각, 도착 시각을 입력하고 **Build playback check**를 누르면 재생용 전환·연속 미리보기·고정 3상태 미리보기 ABP·전체 89본 경계 보고서를 생성합니다. 이 창은 기존 결과를 검사하며 새 모델 추론을 실행하지 않습니다.

검증 예제는 Source `MM_Walk_Fwd`, Destination `MM_Run_Fwd`, Generated `AN_Manny_Output23`, N `20`, 출발 `1.0초`, 도착 `0.0초`입니다. N 초기값은 개인 생성 설정을 읽으므로 현재 설정이 15라면 예제 검사에서만 20으로 바꿉니다. 입력 에셋과 시각의 출처를 자동 추론하지 않으므로 실제 생성 요청과 맞춰야 합니다. 모든 클립은 같은 Manny Skeleton의 비가산 Animation Sequence여야 합니다.

출력은 `/Game/MotionInbetweening/PlaybackChecks/Check_<고유 ID>/`에 저장합니다. 기존 원본·사용자 ABP를 덮어쓰지 않습니다. 보고서는 `Saved/MotionInbetweening/PlaybackChecks/<고유 ID>/report.json`이며 실패 전 저장된 파생 에셋도 기록합니다. 창의 **Open continuous preview**, **Open preview ABP**, **Open report folder**로 결과를 엽니다.

재생용 전환은 전체 결과의 0부터 시작하는 인덱스 9부터 마지막까지 N+2키를 사용합니다. N=20이면 22키·0.7초입니다. 연속 미리보기는 원본의 출발 전 0.3초, 전환, 도착 후 0.5초를 이어 붙이며 각 접점의 중복 키를 제외합니다. 원본 클립에 이 구간이 없으면 생성 전에 거부합니다. 미리보기 ABP는 `Source → Bridge → Destination`을 한 번 재생하고 마지막 자세에 머뭅니다.

보고서의 success는 에셋·ABP 생성 성공이며 경계 품질 승인이 아닙니다. 루트·골반 좌표와 비모델 본의 차이를 그대로 측정하므로 기존 샘플에서 큰 오차가 나타날 수 있습니다. 좌표 보정·자세 복원, 양방향 반복 연결, 동작 다중 선택·자동 변환·그래프·일괄 추론·RequestedMotion 게임 ABP는 후속 구현 범위입니다. cook/패키지 검증은 아직 하지 않았습니다.

자동화 검사는 `MotionInbetweeningUX.Playback.Create`와 새 에디터 프로세스의 `MotionInbetweeningUX.Playback.Reload`입니다. Create가 남긴 보고서 경로를 Reload가 읽으며, 기본 위치는 `Saved/MotionInbetweening/PlaybackCheckTests/created-report.txt`입니다. `MIB_PLAYBACK_RECORD_DIR` 환경 변수로 검사 기록 위치를 지정할 수 있습니다.

</details>
