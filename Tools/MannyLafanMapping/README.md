# Manny → LAFAN1 입력 매핑

UE5 Manny 애니메이션을 LAFAN1 기반 Motion In-betweening 모델이 사용하는 프레임당 135차원 입력으로 변환하는 도구입니다.

## 현재 구현 범위

- Manny에서 사용하는 22개 관절과 LAFAN1 관절 순서 대응
- UE 좌표계와 LAFAN1 좌표계 변환
- 관절 회전을 6D 회전 표현으로 변환
- `22 × 6D 회전 + Hips 위치 3개`로 135차원 상태 생성
- Context Transformer의 원점·진행 방향 정렬 및 정규화 전처리
- 변환 왕복 및 입력값 자동 검증

현재는 `Manny → LAFAN1 모델 입력` 방향까지 구현되어 있습니다. 모델 출력의 `LAFAN1 → Manny` 역매핑은 후속 작업입니다.

`rest_pose_corrections_22_prototype.json`은 현재 검증에 사용한 보정값입니다. 입력 파이프라인과 수치 왕복 검사는 통과했지만, 한 기준 자세에서 산출한 값이므로 최종 애니메이션 품질을 확정하기 전에는 시각 검증이 필요합니다.

테스트용 `manny_mm_idle_frames_0_9.json`의 에셋 경로는 최초 추출 프로젝트의 기록입니다. 테스트에서는 저장된 관절값만 사용합니다. 새 데이터를 추출할 때는 `Unreal/` 스크립트에 설정된 현재 저장소의 `/Game/ControlRig/...` 경로가 사용됩니다.

## 구성

- `manny_lafan_transform.py`: 관절 대응, 좌표계, 회전행렬 및 6D 회전 변환
- `convert_manny_sequence_135.py`: UE에서 추출한 임의의 Manny 시퀀스를 raw 135차원 상태로 변환
- `official_context_preprocess.py`: 모델 입력의 방향 정렬, 정규화와 역변환
- `rest_pose_corrections_22_prototype.json`: 22관절 기준 자세 보정값
- `manny_mm_idle_frames_0_9.json`: 자동 테스트용 최소 Manny 샘플
- `test_mapping_v2.py`: 매핑 회귀 테스트
- `test_context_preprocess.py`: 전처리 왕복 테스트
- `Unreal/`: UE Editor Python 환경에서 실행하는 Manny 추출 스크립트

UE 추출 스크립트의 기본 에셋 경로는 이 저장소의 `Content/ControlRig` 구조에 맞춰져 있습니다. 프로젝트에서 에셋 위치를 옮긴 경우 스크립트 상단의 경로 상수를 변경해야 합니다.

## 테스트

```bash
python -m pip install -r requirements.txt
python -m unittest test_mapping_v2.py test_context_preprocess.py
```

UE에서 Walk를 추출한 뒤 변환하는 예시는 다음과 같습니다.

```bash
python convert_manny_sequence_135.py Unreal/manny_walk_fwd_sequence.json -o manny_walk_fwd_135_raw.json
```

모델 체크포인트, LAFAN1 전체 데이터셋, 생성 결과 JSON 및 보고서 파일은 포함하지 않습니다.


