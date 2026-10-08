# 플러그인 추론 연결 제안 v1

2026-10-08 이영호 측 호출기 구현에 맞춘 **협의용 규격**입니다. 서준영의 기존 추론 구현을 확인해 조정하며, 팀 전체가 합의한 최종 사양이나 실제 모델 연결 완료를 뜻하지 않습니다. 모델 코드는 아직 이 경로에 수신되지 않았습니다.

## 전달할 위치와 내용

호출기는 현재 [codex/plugin-inference-runner 브랜치](https://github.com/moumee/ue5-transition-anim-generator/tree/codex/plugin-inference-runner)에 공유했습니다. **개발 중인 연결 작업이며 main에는 반영하지 않았습니다.** 실제 모델 코드·샘플을 받아 연결을 확인할 예정입니다.

기존 저장소에서 아래 명령으로 호출기 브랜치를 가져와 별도 기능 브랜치에서 작업할 수 있습니다. `feature/model-inference-entry`는 예시 이름이며 기존 작업이 있으면 새 이름을 사용합니다.

```bash
git fetch origin
git switch -c feature/model-inference-entry origin/codex/plugin-inference-runner
```

새로 받는 경우에는 [저장소](https://github.com/moumee/ue5-transition-anim-generator)를 clone할 때 `--branch codex/plugin-inference-runner`를 지정하면 됩니다. 로컬 수정이 있으면 그 작업을 보존한 뒤 브랜치를 전환합니다.

추론 코드는 `Tools/MotionInference/`에 작업하고, 연결용 PR의 **base는 `codex/plugin-inference-runner`**로 지정해 전달하는 방식을 제안합니다. 실제 모델 연결·검증을 마치기 전에는 main에 병합하지 않습니다.

- `infer.py`: 기존 추론 함수를 호출하는 진입점. 아래 `--request` JSON을 읽고 지정한 파일에 결과를 씁니다. 기존 CLI가 있으면 먼저 실행법과 샘플을 공유해도 됩니다.
- `requirements.txt`와 실행 안내: 확인한 Python·PyTorch·CUDA 버전, CPU/GPU 지원, 환경 준비 및 재현 명령.
- 모델 설정 예제: 체크포인트·통계 파일을 지정하는 JSON. 설정 키는 모델 담당자가 정의하며 상대 경로는 **설정 파일의 폴더 기준**으로 해석합니다.
- 재현 샘플: 실제 시작/도착 `model_input.json`, 실행에 사용한 요청·설정, 생략 없는 출력 JSON 1세트와 사용 코드 커밋. 출력의 `inference_request_id`는 해당 요청과 일치해야 합니다.
- 대용량 가중치·통계: 공유 위치와 SHA-256, 대응 모델/버전. 대용량 파일을 Git에 직접 추가하는 것을 요구하지 않습니다.

Unreal의 입력 내보내기, 외부 프로세스 실행·취소·시간 제한, 출력 검증·역매핑·에셋 저장은 플러그인에서 담당합니다. `Tests/Fixtures/inference_fixture_backend.py`는 저장된 포즈를 반복하는 **호출 시험 전용**이며 실제 추론 구현의 예제가 아닙니다.

## 호출

```text
<inference-python> -X utf8 -B <infer.py> --request <request.json>
```

각 경로는 별도 인수로 전달하며 셸을 거치지 않습니다. cwd는 `infer.py`의 폴더입니다. 요청 안의 파일 경로는 절대 경로이며 공백·한글을 포함할 수 있습니다. 입력은 읽기 전용으로 취급하고, `output_file`에 UTF-8 JSON을 완성한 뒤 종료 코드 0으로 끝냅니다. 실패하면 stderr에 원인을 쓰고 0이 아닌 코드로 종료합니다. 플러그인은 stdout/stderr를 작업 폴더에 보관합니다. 별도 분리 프로세스/서버를 띄우지 않는 동기 실행을 전제로 합니다.

## request.json

기본 전환 길이 20샘플과 31샘플짜리 입력 파일을 사용한 예시입니다. 경로·ID·해시는 설명용이며 실제 실행 때 플러그인이 채웁니다.

```json
{
  "schema": "motion_inbetweening.inference_request.v1",
  "request_id": "unique-job-id",
  "start_input_file": "C:/job/start_input.json",
  "end_input_file": "C:/job/end_input.json",
  "output_file": "C:/job/model_output.json",
  "model_config_file": "C:/models/config.json",
  "sample_rate_hz": 30,
  "context_frames": 10,
  "start_context_start_index": 21,
  "end_target_index": 0,
  "transition_frames": 20,
  "expected_output_samples": 31,
  "source_sha256": {"start": "<sha256>", "end": "<sha256>"}
}
```

시작/도착 파일은 플러그인의 `motion_inbetweening.manny_input.v1` 형식입니다. `vectors_tx135`는 22관절의 부모 기준 rotation6D 132값 + Hips XYZ 3값입니다. joint 순서·parents·6D 배열 규칙과 cm 단위는 `Tools/MannyLafanMapping`의 기존 변환을 따릅니다. 정규화된 수치 자체에는 cm 단위를 직접 적용하지 않습니다. 입력마다 `normalization_applied`, `alignment`, `joint_order`, `parents`, `sample_count`, `sample_rate_hz`, `context_len`이 있습니다.

| 구간 | 선택과 출력 인덱스 |
| --- | --- |
| 시작 문맥 | 시작 입력의 마지막 10샘플. 출력 `[0..9]` |
| 생성 구간 | 마지막 문맥과 도착 목표 **사이**의 N샘플. 출력 `[10..9+N]` |
| 도착 목표 | 도착 입력의 첫 샘플. 출력 `[10+N]` |
| 총 길이 | `10 + N + 1`. N=20이면 31샘플·30fps·1초 |

현재 UI는 내보낸 두 파일을 선택합니다. 원본 애니메이션의 임의 시간 구간 선택·목표 이동 거리/회전 조정 UI는 없습니다. 입력 내보내기는 기본적으로 원본 시작부터 최대 31샘플을 사용합니다. 따라서 ‘시작의 마지막 10샘플’은 **내보낸 구간** 기준입니다. 모델별 N 지원 범위가 다르면 모델 담당자가 지원 길이를 알려 주고 지원하지 않는 요청을 오류로 처리해야 합니다. 호출기 범위는 1~240이며 모델이 모두 지원한다는 뜻이 아닙니다.

## 좌표계·정규화와 출력

두 입력은 각각 독립적으로 heading 정렬되어 있습니다. 도착 배열을 그대로 시작 배열 뒤에 붙일 수 없습니다. 모델 진입점에서 도착 목표를 **시작 입력의 좌표계**로 맞춰 추론하고, 결과도 그 좌표계로 내보내야 합니다. 필요하면 기존 정렬 정보를 역으로 적용해 원래 공간을 복원한 다음 시작 정렬을 적용합니다. 목표 위치·방향 정책은 실제 걷기→달리기 샘플로 함께 확인해야 합니다.

정규화된 입력은 같은 통계 파일·SHA-256이어야 하며 플러그인이 이를 확인합니다. 입력이 정규화되지 않았다면 모델 진입점에서 대응 통계로 정규화합니다. 좌표 변환이 필요할 때는 먼저 물리 수치로 역정규화해야 합니다. 출력의 `normalization_applied`는 **실제 출력 상태**를 나타냅니다. 이미 역정규화한 출력은 false로 쓰며 입력 값을 무조건 복사하지 않습니다.

출력 필수 조건:

- `inference_request_id`: 요청의 `request_id` 그대로.
- `predictions_tx135` (또는 `output_tx135`/`vectors_tx135` 중 하나): 생략 없는 `[expected_output_samples, 135]` 유한한 JSON 숫자 배열.
- `frame_count`: `expected_output_samples`와 같은 정수. `sample_rate_hz`: 30.
- `position_space`: `lafan_start_centered`.
- `alignment.position_offset_xz`와 `alignment.rotation_offset`: **시작 입력의 값을 그대로 보존**. 평탄한 `heading_position_offset_xz`/`heading_rotation_offset` 쌍도 지원합니다. `root_position_offset_lafan`은 붙이지 않습니다.
- `normalization_applied`: bool. true이면 `statistics.sha256`도 필수이며 플러그인의 Training Statistics와 일치해야 합니다.

`joint_order`와 `parents`도 함께 보존하는 것을 권장합니다. 추가 의미 선언·shape·시간 배열이 있으면 기존 [출력 검증 규칙](../../Plugins/MotionInbetweeningHello/README.md)에 맞아야 합니다. NaN/Inf, 숫자 문자열/bool/null, 중복 JSON 키, 요청 ID·길이·heading 불일치는 에셋 생성 전에 거부합니다. 구조 검증만으로 시작/도착 포즈의 의미나 모션 품질을 확인할 수 없으므로 실제 모델의 경계 포즈·방향·발 접촉은 별도 확인합니다.

## 연결 확인 순서

1. 위 자료를 받아 팀 코드의 독립 실행으로 샘플을 재현합니다.
2. 플러그인 설정에서 모델 Python·infer.py·모델 설정·통계를 지정합니다.
3. `Tools → Generate Manny Transition from Model Inputs`에서 START와 DESTINATION 입력을 순서대로 선택합니다.
4. `Saved/MotionInbetweening/Inference/<ID>/pipeline_result.json`의 `success=true`, `stage=completed`, `asset_path`를 확인합니다. Python `result.json` 성공만으로 에셋 저장 완료를 판정하지 않습니다.
5. 실제 걷기→달리기 에셋을 재생해 경계 연결·팔/발 방향·미끄러짐을 확인합니다.

2026-10-08 현재 호출기와 시험용 실행기는 준비했으며 실제 추론 진입점·샘플은 로컬 수신 대기입니다. 이는 준영이 측의 구현 미완료를 뜻하지 않습니다.
