# 출력 가져오기 검사용 모델 결과

`context_model_output.json`은 2026-10-05 PR #2 검토에서 보관한 공식 pretrained Context 모델의 실제 31프레임·135차원 출력 사본입니다. 정규화는 해제됐으며 위치·방향 복원 정보를 포함합니다. 실행 때 모델을 다시 돌리지 않습니다.

원본: `C:/Users/young/prg/School/게임 공학/검토/2026-10-05_PR2/context_model_output.json`

SHA-256: `ffb5b39960c056f45f43623d55d09c3c707f99c0d06d3c718ae4452a63ebf0e1`

추론 실행 근거: 같은 검토 폴더의 `verify_inverse.py`, `independent_verification.json`. 해당 검토는 `post_process=False`를 사용했습니다. 이 fixture가 팀원의 플러그인용 최종 추론 전달 형식 또는 모션 품질 승인을 뜻하지 않습니다.
