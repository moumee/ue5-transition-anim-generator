# Manny 실제 모델 추론

`infer.py`는 플러그인의 `motion_inbetweening.inference_request.v1` 요청을 받아 공식 Context + Detail 가중치로 전환 구간을 생성합니다. Context 단독 경로도 지원합니다. 새 학습·데이터셋·Visdom 서버 없이 CPU에서 동기 실행하며, 오류는 stderr와 0이 아닌 종료 코드로 반환합니다.

## 준비

Python 3.11 환경에서 이 폴더의 `requirements.txt`를 설치합니다.

```text
python -m pip install -r requirements.txt
```

공식 [사전 학습 파일](https://github.com/victorqin/motion_inbetweening/releases/download/v1.0.0/pre-treained.zip)을 받아 다음 파일을 준비합니다. 기본 예제는 이 폴더의 `models/` 아래에 각 모델 폴더를 둡니다. 다른 위치를 사용할 때는 설정 경로를 바꿀 수 있습니다.

- `lafan1_context_model_release/checkpoint_lafan1_context_model_release.pth`
- `lafan1_context_model_release/train_stats_context.pkl`
- `lafan1_detail_model_release/checkpoint_lafan1_detail_model_release.pth`
- `lafan1_detail_model_release/train_stats_detail.pkl`

`model_config.example.json`을 `model_config.local.json`으로 복사하고 경로를 지정합니다. 상대 경로는 **설정 파일의 폴더 기준**입니다. 진입점은 `model_manifest.json`의 파일 SHA-256과 대조한 뒤 가중치·통계를 읽고, 모델 형상은 strict 모드로 확인합니다. 가중치·개인 설정은 Git에 포함하지 않습니다. 공식 모델과 다른 후보는 설정의 경로만 바꿔 암묵적으로 수용하지 않으며 별도 버전·통계·검증이 필요합니다.

추론 함수와 모델 클래스는 `vendor/`에 원본 그대로 보관했습니다. 팀 소스 `3d2ba85e4678a5043d61b0be99763b086afa9dc4`, 상류 `fa9b6dc5f0791fd28bfccb6783e6bfd26d578515`를 기준으로 하며 변경은 학습/손실 경로에 한정되고 이 버전의 추론 함수는 상류와 같습니다. 원본 MIT 고지와 파일 해시는 `vendor/LICENSE.txt`, `vendor/source_manifest.json`에 있습니다. 학습용 Config·통계 생성·체크포인트 로더는 호출하지 않습니다.

## Unreal에서 사용

1. 플러그인 설정의 외부 Python을 준비한 환경으로 지정합니다. Training Statistics를 사용할 경우 위 Context 통계를 지정합니다.
2. 시작·도착 Manny 애니메이션을 각각 입력 내보내기 메뉴로 내보냅니다. 도착 목표의 다음 프레임까지 필요합니다.
3. 모델 Python, Inference Script(`infer.py`), Model Config(`model_config.local.json`)를 설정합니다.
4. `Tools → Generate Manny Transition from Model Inputs`에서 START와 DESTINATION의 `model_input.json`을 순서대로 선택합니다.
5. `Saved/MotionInbetweening/Inference/<ID>/pipeline_result.json`의 `success=true`, `stage=completed`, `asset_path`를 확인하고 생성 에셋을 재생합니다. Python 출력 파일만으로 에셋 저장 성공을 판정하지 않습니다.

기본 Manny의 `MM_Run_Fwd`는 30Hz에서 루트가 샘플당 최대 약 17.98cm 이동합니다. 이 클립을 내보낼 때는 입력 검사 설정의 `Max Root Step Cm`을 20 이상으로 지정합니다. 기본값 15cm에서 거부되는 이유는 측정된 이동량이 제한을 넘기 때문이며, 이 제한을 바꾸는 것이 생성 모션의 품질을 보장하지는 않습니다.

이 어댑터의 전환 길이는 **5~45샘플**입니다. 이는 구현의 지원 정책이며 설정의 `max_seq_len=65`가 강제하는 한계가 아닙니다. 호출기 자체의 1~240 범위와 구분합니다. 생성 품질은 동작·목표 배치에 따라 달라지므로 저장 성공과 별도로 방향·경계·발 미끄러짐을 확인해야 합니다.

## CLI와 샘플

```text
python -X utf8 -B infer.py --request <request.json 절대경로>
```

요청 필드는 `schema`, `request_id`, `start_input_file`, `end_input_file`, `output_file`, `model_config_file`, `sample_rate_hz=30`, `context_frames=10`, `start_context_start_index`, `end_target_index`, `transition_frames`, `expected_output_samples`, `source_sha256`입니다. 파일 경로는 절대 경로이며 한글·공백을 지원합니다. 플러그인이 입력 사본과 요청을 만듭니다. 기존 출력 파일은 덮어쓰지 않습니다.

`samples/start_input.json`과 `samples/end_input.json`은 실제 Manny Walk·Run에서 내보낸 생략 없는 비정규화 입력입니다. `run_sample.py --config <개인 설정> --output-dir <새 폴더>`는 이 입력의 해시와 절대 경로를 넣은 요청을 생성하고 같은 CLI를 실행합니다. 자동으로 Unreal 에셋을 생성하지는 않습니다.

## 처리 규약

- 외부 결과는 문맥 10 + 생성 N + 도착 목표 1입니다. 내부에는 도착 목표 다음 프레임을 추가해 `10+N+2`를 만들고 후처리 후 마지막 보조 프레임을 제외합니다. 목표를 복제해 속도를 꾸미지 않습니다.
- 각각 정렬된 두 입력을 시작 입력 좌표계로 맞춥니다. 선택한 10개 문맥의 마지막 프레임으로 모델 좌표계를 다시 정렬하고, 출력은 시작 입력 좌표계로 되돌립니다. 시작 `alignment`는 원문 그대로 반환합니다.
- `target_placement=endpoint_velocity`가 기본입니다. 도착 heading을 마지막 문맥에 맞춘 뒤, 시작 문맥의 마지막 3개 수평 이동량 평균과 도착 목표→다음의 실제 수평 이동량을 평균해 N+1 구간의 목표 거리를 정합니다. 도착 클립의 목표→다음 변화는 보존합니다. 방향이나 속도가 다른 두 클립의 목표를 자동 배치하는 정책이며 in-place 클립에 임의 보행 속도를 만들지는 않습니다.
- 이 기본 정책에서는 모델 후처리 뒤 생성 구간의 수평 위치에 양 끝 잔차를 선형 보간해 더합니다. 첫 생성 이동량은 마지막 문맥 이동량, 마지막 생성→목표 이동량은 목표→다음 이동량과 일치합니다. 알려진 포즈·수직 위치·모델 회전은 유지합니다. 출력의 `horizontal_boundary_correction`에 실제 보정량을 남깁니다. 발 접촉을 고정하는 IK나 품질 보장 기능은 아닙니다.
- `continue_context`는 이전 정책입니다. 시작 문맥의 마지막 3개 수평 이동량 평균만 N+1 구간 연장합니다. 서로 다른 속도의 Walk→Run 등에서는 중간 감속과 끝 이동량 급증이 생길 수 있습니다. 기존 결과를 재현하려고 명시한 설정은 자동으로 새 정책으로 바꾸지 않습니다.
- `preserve_exported`는 두 입력의 정렬을 복원한 원래 배치를 유지합니다. 서로 다른 원점에서 만들어진 이동형 클립에는 되돌아가는 목표를 만들 수 있으므로 배치를 확인해야 합니다. 실제 사용 정책은 출력 메타데이터에 기록됩니다.
- 정규화 입력은 지정 Context 통계 해시로 검증·역정규화합니다. 물리 좌표에서 변환·정적 채널 투영을 한 후 기존 `evaluate`가 모델별 정규화·역정규화를 수행합니다. 출력은 항상 `normalization_applied=false`입니다.
- LAFAN 통계에서 정적인 발가락 12채널은 모델 입력에서 평균값으로 투영합니다. 새 내보내기는 `unprojected_static_rotations`에 투영 전 회전을 함께 보존합니다. 결과의 비생성 프레임은 원래 포즈를 보존하고, 생성 구간의 해당 관절은 원래 양 끝 회전 사이를 SLERP합니다. 모델이 발가락 동작을 학습했다고 주장하지 않습니다.
- 정규화된 옛 입력에 투영 전 회전이 없으면 재내보내기를 요청하고 실패합니다. 역정규화만으로 원래 발가락 값을 복원할 수 없습니다. 비정규화 입력은 원본 회전이 남아 있어 이 추가 필드 없이 사용할 수 있습니다.
- Detail 마스크는 키워드 인수로 전달하며 접촉 텐서는 입력과 같은 dtype/device를 사용합니다. Context와 Detail 모두 후처리를 켭니다. 접촉 확률은 생성 구간에 대해서만 기록합니다.

출력에는 실제 요청·설정 스냅샷, 모델/통계/코드 해시, 환경 버전, 내부 길이, 목표 정책과 끝점 오차·경계 이동량 등의 진단값이 포함됩니다. 형상·유한값·포즈 보존 검사가 시각 품질을 대신하지는 않습니다.

`alignment.reference_frame`은 원래 시작 입력을 정렬할 때의 입력 프레임 번호입니다. 출력의 문맥 시작 번호와 혼동하지 않도록 `start_context_start_index`와 `alignment_reference`를 함께 기록합니다. 속도 진단은 XZ 수평 cm/샘플, 기존 `boundary_root_steps_cm`은 3차원 이동량입니다. 큰 정규화 값은 알려진 입력의 프레임·채널·평균/표준편차와 함께 남기며 임의 임계값으로 품질 합격을 판정하지 않습니다.

## 검사

```text
python -m unittest discover -s Tests -v
```

기본 검사는 알려진 월드 좌표로 만든 서로 다른 원점·yaw 입력의 선택 문맥/목표/다음 프레임과 생성 역변환을 세 배치 정책에서 독립 기대값과 비교합니다. 경계 수평 이동량·회전·발가락 보간도 확인합니다. 공식 가중치 통합 검사는 `MIB_MODEL_CONFIG` 환경 변수에 준비한 설정의 절대 경로를 지정해 실행합니다. 가중치가 없으면 해당 검사는 명시적으로 skipped이며 통과로 세지 않습니다. 모델 검사는 지원 길이, 서로 다른 목표, 정규화 동등성, 원본 발가락 보존, 잘못된 통계와 옛 입력 거부를 포함합니다. 기본 Context + Detail 경로의 수치 기준선은 버전 변화 탐지용이며 품질 승인을 뜻하지 않습니다. 기존 플러그인 Python·UE 자동화와 실제 에셋 저장·재로딩은 별도로 실행합니다.
