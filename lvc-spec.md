# LVC (Local VS Code) — 스펙 & 개발 플랜

macOS 전용 네이티브 AI 코드 에디터. 로컬 Ollama 모델 로드 + 코딩 특화 파인튜닝 지원.

> **방향 전환 (2026-10-12)**: 파인튜닝 라인은 보류(파크). `lvc-coder` 모델은 Ollama에서 삭제했고,
> 대신 Claude Code 스타일 터미널 에이전트 **`lvcli`** 가 새 주력 (`make lvcli`, `./lvcli "과제"` 또는 REPL).
> qwen2.5-coder:3b 등 일반 instruct 모델을 도구 루프로 구동. 파인튜닝 산출물(데이터셋/스크립트/평가 하네스)은
> `finetune/`에 재현 가능한 형태로 보존 — 재개 시 `finetune/README.md` 참고.

---

## 1. 파인튜닝 방법 (리서치 결과)

채택 파이프라인: **Training → LoRA/QLoRA → Merge → GGUF 변환 → 양자화 → Ollama Modelfile → `ollama create`**

| 단계 | 도구 | 비고 |
|---|---|---|
| 학습 | Unsloth + QLoRA (Python/Colab 또는 로컬 MLX) | VRAM 절감, Mac에선 MLX-LM도 가능 |
| 병합 | `peft.merge_and_unload()` 또는 `llama-export-lora` | 어댑터 → 단일 모델 |
| 변환 | `llama.cpp/convert_hf_to_gguf.py` | HF → GGUF |
| 양자화 | `llama-quantize` (Q4_K_M 기본) | Apple Silicon 통일 메모리에 최적 |
| 등록 | `Modelfile` + `ollama create my-model -f Modelfile` | 에디터에서 `my-model` 로드 |

- **코딩 특화 데이터**: FIM(Fill-In-the-Middle) 형식으로 학습 — prefix/suffix/ middle 구조. 접수된 tab-completion 수집 → 데이터셋화 → 재학습 루프 (acceptance rate 측정).
- 자동완성용은 **소형 FIM 모델(3B급, 예: Qwen2.5-Coder / StarCoder2 3B 베이스)**, 채팅용은 별도 중형 모델. 범용 chat 모델은 FIM 성능이 나쁨 (검증된 사실).

## 2. 내장 스킬 (에디터 기능)

- **Tab 자동완성 (FIM)**: 커서 앞뒤 컨텍스트 → Ollama `/api/generate`, ghost-text UI
- **인라인 편집/리팩토링**: 선택 영역 → 지시 프롬프트 → diff 프리뷰 → accept/reject
- **채팅 사이드바**: 파일/선택 영역 컨텍스트 첨부, 스트리밍 응답
- **코드 설명 / 커밋 메시지 생성 / 에러 해석**
- **프로젝트 인덱싱(RAG)**: 임베딩 모델 + 벡터 검색으로 컨텍스트 주입
- **수집 루프**: 채택된 제안 로그 → 파인튜닝 데이터셋 자동 생성

## 3. 기술 스택 (macOS 최적화)

| 레이어 | 기술 | 이유 |
|---|---|---|
| 코어 (버퍼, 편집, LSP 클라이언트, Ollama HTTP 클라이언트) | **C** (요구대로) | 이식성·속도, 없는 의존성 |
| 렌더링 | **Metal (metal-cpp)** 또는 1차 버전은 CoreText + AppKit | GPU 텍스트 렌더링; Electron 대비 RAM 1/5, 전력 10~20x 절감 |
| UI 셸 | **Objective-C / AppKit** (C에서 Objective-C 런타임 호출) | 네이티브 메뉴바·다크모드·접근성 |
| 추론 | **Ollama (localhost:11434 REST)** — 요구대로 외부 프로세스 | 직접 llama.cpp 내장 대신 IPC로 분리 |
| 구문강조 | Tree-sitter (C 라이브러리) | C 생태계 그대로 |

> C 대체 허용 시: UI 셸을 Swift/AppKit으로 하고 C 코어를 링크하는 하이브리드가 생산성 최선. 순수 C 유지는 가능하나 UI 개발 속도 크게 손해.

## 4. 최소 사양 — MacBook Neo (2026년 3월 출시, $599) 기준

MacBook Neo 실측 사양: **Apple A18 Pro, 통일 메모리 8GB LPDDR5X-7500, 256/512GB SSD, 13" Liquid Retina 2408×1506, macOS 최신 (Metal 3 지원)**

- **A18 Pro + 8GB에서의 타깃 모델**:
  - 자동완성(FIM): 0.5B~3B Q4_K_M (예: Qwen2.5-Coder 1.5B/3B) — 메모리 ~1–2GB, A18 Pro NPU/GPU 무관, Ollama가 CPU+GPU 혼용
  - 채팅: 3B~7B Q4 한정. 7B Q4(~4.5GB) 로드 시 다른 앱 여유 메모리 부족 → 기본값은 3B 권장
  - 14B 이상은 Neo 사양에서 비권장 (스왑 발생)
- 앱 자체 예산: RSS **150MB 이하**, 입력 지연 <16ms (60fps), 유휴 CPU 0%
- 디스크: 256GB 기준 모덐 저장 10GB 이내 권장
- 상위 기기(M 시리즈, 16GB+)에서는 동일 앱이 모델 선택만으로 자동 확장 — 앱은 메모리 크기 감지해 모델 추천
- Ollama 앱 사전 설치 필수

## 5. 개발 플랜 (마일스톤)

**M1 — 에디터 코어 (C)**: gap-buffer 텍스트 버퍼, 파일 I/O, UTF-8, undo/redo, Tree-sitter 강조 ✅ (증분 line index 포함)
**M2 — UI 셸**: AppKit 윈도우 + 탭/사이드바, CoreText 텍스트뷰 → 이후 Metal 렌더러로 교체 ✅ (화살표 내비게이션, CTLine 정밀 캐럿; Metal은 LVC_RENDERER=metal)
**M3 — Ollama 연동**: REST 클라이언트(스트리밍), 모델 목록/선택 UI, 채팅 사이드바 ✅ (RAM 기반 모델 추천 포함)
**M4 — FIM 자동완성**: debounce 요청, ghost text, accept/reject, 수집 로그 ✅ (멀티라인 ghost·줄단위 Tab, sanity 필터, brace-depth suffix, 크로스파일 헤더 컨텍스트, ctx 로깅)
**M5 — LSP + 인라인 편집**: clangd 등 LSP 클라이언트, diff 기반 인라인 리팩토링 ✅ (clangd 진단 폴링 표시)
**M6 — 파인튜닝 파이프라인**: 수집 데이터 → 학습 스크립트 → GGUF 변환 → `ollama create` 자동화 ✅ (MLX LoRA 표준 설정 `finetune/lora_config.json`, AST/문장경계 마이너 + 재현가능 코퍼스, `eval_fim.py` 벤치마크, DPO 페어 변환기)
**M7 — 최적화/배포**: Metal 렌더러 전환, .app 번들/공증, 벤치마크 (입력 지연 <16ms, 제안 TTFB <300ms) — 진행 중 (make ci: ASAN/퍼저/테스트 일괄)

### 에이전트 모드 (2026-10-03 추가)
채팅 입력을 `!`로 시작하면 에이전트 모드: Ollama `/api/chat` 도구 루프(list_dir / read_file / edit_file / run_shell)로 다단계 작업 수행. 이벤트는 채팅 사이드바에 스트리밍. `src/ai/agent.c`, 테스트 `tests/test_agent.c` (mock 서버 11436). cwd는 열린 파일의 디렉터리로 제한, run_shell 30초 타임아웃.

### 채팅 품질 설정 (MCF.scpt 참고, 2026-10-12)
- 채팅/리팩토링 요청: `temperature 0.2, repeat_penalty 1.15` — 반복·환각 억제 (FIM과 동일 처방)
- 대화 기록: `~/.lvc/chat_history.txt` 자동 저장 (질문/답변/타임스탬프)
- 에이전트 페르소나: "간결한 한국어 설명 + 깔끔한 코드"

## 참고 출처
- Unsloth Ollama 파인튜닝 튜토리얼: https://unsloth.ai/docs/get-started/fine-tuning-llms-guide/tutorial-how-to-finetune-llama-3-and-use-in-ollama
- LoRA→GGUF 변환: https://github.com/hrishi-008/LoRA-adapter-to-GGUF-for-Ollama-with-code
- Ollama + Continue 아키텍처: https://ollama.com/blog/continue-code-assistant
- FIM 기법: https://vnavarro.dev/blog/fim , https://arxiv.org/abs/2412.16589
- 네이티브 vs Electron: https://openmarkapp.com/blog/native-mac-apps-vs-electron
- metal-cpp: https://developer.apple.com/videos/play/wwdc2022/10160/
