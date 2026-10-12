# LVC Fine-tuning Pipeline

> **상태: 파크 (2026-10-12)** — 파인튜닝 라인 중단, `lvc-coder` 모델 삭제됨.
> 파이프라인/데이터/평가 하네스는 재현 가능하게 보존. 재개 시 아래 순서대로.
> 현재 주력은 `lvcli` (일반 instruct 모델 + 도구 루프) — `src/cli/main.c`.

에디터에서 수집된 제안 로그 → FIM 파인튜닝 → Ollama 등록까지의 전 과정.

```
editor (제안 수집 JSONL)
  → build_dataset.py        # FIM 예제 변환 (Qwen 토큰)
  → train_fim.py            # Unsloth QLoRA 학습 + GGUF 병합/양자화
  → ollama_register.py      # Modelfile 생성 + ollama create
  → 에디터에서 모델 선택
```

## 사전 준비

```bash
# 1. 파인튜닝은 GPU 권장(무료 Colab T4 가능). Mac에서는 1.5B 4bit로도 동작.
pip install unsloth datasets trl

# 2. llama.cpp (GGUF 변환/양자화 도구)
git clone --depth 1 https://github.com/ggerganov/llama.cpp vendor/llama.cpp
cmake -S vendor/llama.cpp -B vendor/llama.cpp/build && cmake --build vendor/llama.cpp/build -j

# 3. Ollama 실행 중이어야 등록 가능
```

## 실행

```bash
# 0) 코퍼스 수집 (11개 레포 shallow clone → finetune/corpus/, 재실행 시 재사용)
python3 finetune/fetch_corpus.py

# 1) 데이터셋 생성 — AST/문장경계 컷 + 중복 제거, ~21k 예제 (c/py/html)
python3 finetune/mine_github_dataset.py     # → finetune/data/{train,valid,test}.jsonl

# 2) 학습 + GGUF 내보내기 — 표준 설정은 finetune/lora_config.json (mask_prompt 필수)
mlx_lm lora --config finetune/lora_config.json
#    체크포인트는 greedy 프로브로 행동 검증 후 최선 선택 (반복 루프 붕괴 이력 있음)
mlx_lm fuse --model <base> --adapter-path finetune/adapters \
    --save-path finetune/merged --dequantize
python3 vendor/llama.cpp/convert_hf_to_gguf.py finetune/merged \
    --outfile finetune/lvc-coder-fim-q8.gguf --outtype q8_0

# 3) Ollama 등록
ollama create lvc-coder -f finetune/Modelfile

# 4) 오프라인 벤치마크 (HumanEval-Infilling, exact-match)
python3 finetune/eval_fim.py --model lvc-coder:latest --model qwen2.5-coder:3b
```

## 수집 로그 → 재학습 루프

```bash
# 에디터 로그(~/.lvc/suggestions.jsonl, ctx 포함)에서 SFT 데이터
python3 finetune/build_dataset.py ~/.lvc/suggestions.jsonl -o fim_dataset.jsonl

# accepted/rejected가 같은 ctx에 모이면 DPO 페어 생성
python3 finetune/build_dataset.py ~/.lvc/suggestions.jsonl --pairs -o dpo_pairs.jsonl

# 채택률 리포트
make accept-report   # 또는 python3 scripts/accept_report.py
```


## 데이터 형식

수집 로그(`accepted` 필드) 에디터가 자동 기록:
```json
{"ts":1727000000,"lang":"c","suggestion":"return x + 1;\n","accepted":true}
```

데이터셋(학습 입력):
```json
{"prompt":"<|fim_prefix|>...<|fim_suffix|>...<|fim_middle|>","completion":"return x + 1;"}
```

## 팁
- 8GB 기기: `--base`는 반드시 ≤3B. 학습 자체는 Colab에서 하고 GGUF만 로컬로 가져와도 됨.
- OOM 시: `--lora-r 8`, `--bs 1 --grad-accum 8`.
- 제안 채택률(acceptance rate)로 개선 측정: `grep -c '"accepted":true' log` 비율 추적.

## 속도 향상 파인튜닝 — GitHub 참고 (2026-09 조사)

### 기법 → LVC 적용
| 기법 | 출처 | LVC 적용법 |
|---|---|---|
| **증류(Distillation)** — 큰 teacher의 출력으로 작은 student 학습 | [bananamort/luau-qwen2.5-coder-1.5b-distillation](https://github.com/bananamort/luau-qwen2.5-coder-1.5b-distillation) | 3B(base)가 만든 FIM 제안을 1.5B student로 학습 → 토큰 생성 속도 2배, 메모리 1GB 이하 |
| **rsLoRA + QAD(양자화 인지 증류)** | 상동 | `train_fim.py`에 `use_rslora=True`, teacher logits 없으면 SFT-on-teacher-outputs로 근사 |
| **IFIM (지시문 결합 FIM)** | [arXiv:2509.24637](https://arxiv.org/pdf/2509.24637) | 데이터셋에 "위 함수 완성" 지시문 + FIM 혼합 학습 → 채팅/자동완성 단일 모델 품질 동시 향상 |
| **양자화 선택** | [SynConfRoute, arXiv:2605.04894](https://arxiv.org/pdf/2605.04894) | **3B는 Q4_K_M**(품질-지연 최적), 1.5B 이하는 **Q8_0** 권장 — 작은 모델을 Q4로 눌러 오히려 나빠지는 것 방지 |
| **하드 네거티브 증류** | [arXiv:2606.03130](https://arxiv.org/pdf/2606.03130) | teacher가 틀린 FIM 사례를 negative로 추가 → 환각(거짓 완성) 감소, 채택률 상승 |

### 벤치마크 목표 (동일 레포/논문 기준)
- 프로덕션 인라인 완성 지연 목표: **200ms 미만**
- 1.5B distill 모델: Q4_0 ~890MB, 2048토큰 프리필 ≤2.3s(CPU) — LVC의 `num_predict 32` 설정과 결합 시 8GB 머신에서도 300ms 내외 도달 가능

### Mac 로컬 학습 경로 (Colab 대안)
| 도구 | 레포 | 비고 |
|---|---|---|
| **mlx-lm** (공식) | [ml-explore/mlx-lm](https://github.com/ml-explore/mlx-lm) | `mlx_lm.lora --model <4bit> --data <jsonl> --iters 1000` — Metal 통일메모리 직접 학습, Mac에서 가장 빠른 학습 경로 |
| **mlxtuner** | [Dolmaa24/mlxtuner](https://github.com/Dolmaa24/mlxtuner) | 한 줄 명령, **8GB Mac 메모리 인지 기본값** |
| **MLX-LoRA-Studio** | [Goekdeniz-Guelmez/MLX-LoRA-Studio](https://github.com/Goekdeniz-Guelmez/MLX-LoRA-Studio) | 네이티브 GUI, QLoRA/QAT/GRPO 등 10종 알고리즘 |
| **mlx-tune** | [ARahim3/mlx-tune](https://github.com/ARahim3/mlx-tune) | Unsloth 호환 API, SFT/DPO/GRPO |

MLX 경로 워크플로: `mlx_lm.lora` 학습 → `mlx.fuse`로 어댑터 병합 + **GGUF 직접 출력** → `ollama create` (llama.cpp 변환 단계 생략 가능).

**이 레포의 표준 학습 설정: `finetune/lora_config.json`** — `mask_prompt: true` 필수 (프롬프트까지 loss에 넣으면 실전 문맥에서 반복 루프 붕괴), scale 16, num_layers 16, 데이터는 빈 completion 행 제거 후 사용. 학습 후: `mlx_lm fuse --model <base> --adapter-path finetune/adapters --save-path finetune/merged --dequantize` → `convert_hf_to_gguf.py --outtype q8_0` → `ollama create`. 체크포인트는 반드시 greedy 프로브(실제 파일 중간 커서 프롬프트)로 행동 검증 후 등록.

### 권장 순서 (LVC 기준)
1. 현재 `3b`로 수집 로그 수집 (acceptance rate 기록)
2. 데이터 수천 건 확보 → **1.5B student 증류** (Q8_0 양자화) — 추론 속도 ~2배
3. 결과가 느리면 MLX 로컬 재학습 루프로 이동 (Colab 불필요)
