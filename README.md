# predator_cli — terminal coding agent (Claude Code-style, local Ollama)

## Quick Start / 快速开始 / 빠른 시작

```bash
# 1. 설치 / install / 安装  (one-time)
git clone https://github.com/kimneche0419-rgb/predator_cli.git
cd predator_cli
make install PREFIX=~/.local/bin

# 2. 실행 / run / 运行  (anywhere)
predator_cli                            # 대화형 REPL / interactive / 交互模式
predator_cli "이 코드베이스 구조 설명해줘"     # 원샷 / one-shot / 单次任务
predator_cli -r ~/.predator/last_session.jsonl  # 세션 복원 / resume / 恢复会话
```

**프로젝트 메모리 & 스킬 / Project memory & skills / 项目记忆与技能**
- `CLAUDE.md`/`AGENTS.md`(작업 디렉터리 또는 상위) → 매 세전 시스템 프롬프트에 자동 주입
- 스킬: `.predator/skills/*.md` (프로젝트) 또는 `~/.predator/skills/` — 시스템 프롬프트에 카탈로그가 실리고 모델이 `read_skill`로 로드
- REPL: `/skills` `/memory` `/reload` `/tools` `/model` `↑↓` 히스토리

사전 준비 / Prerequisite / 前提: Ollama 실행 중 + `ollama pull qwen2.5-coder:3b`

자세한 사용법 / Details / 详细用法: 아래 언어별 섹션 참고

[English](#english) · [中文](#中文) · [한국어](#한국어)

---

## English

### Requirements
- macOS + [Ollama](https://ollama.com) running (`ollama serve` or the menu bar app)
- A chat-capable model pulled, e.g. `ollama pull qwen2.5-coder:3b`
- Build (clang):

```bash
make                        # builds ./predator_cli
make test                   # agent unit tests (mock server e2e)
make install PREFIX=~/.local/bin   # optional: run `predator_cli` from anywhere
```

### One-shot mode
```bash
./predator_cli "explain what src/ai/agent.c does"
./predator_cli -C /path/to/project "fix the compiler warning in foo.c"
./predator_cli -m qwen2.5-coder:3b -H 127.0.0.1:11434 "add error handling to main()"
```

### REPL (interactive)
```bash
./predator_cli
❯ /model                       # list installed models
❯ /model 2                     # switch by name or number
❯ /cwd /path/to/project       # switch workspace
❯ /reset                      # clear conversation memory
❯ /stat                       # transcript size
❯ /quit
```
Anything that is not a `/command` is sent to the model. Conversation memory
persists across turns until `/reset`.

### What the agent can do
It runs a tool loop (like Claude Code) with four workspace-sandboxed tools:

| tool | effect |
|---|---|
| `list_dir` | list a directory |
| `read_file` | read a file (8 KB cap) |
| `edit_file` | replace `old_string` with `new_string` in a file |
| `run_shell` | run a shell command (30 s timeout, 4 KB output cap) |

Paths that escape the working directory are rejected. After ~10 tool rounds the
agent is forced to answer with what it has.

### Flags
```
-m model     chat model (default: qwen2.5-coder:3b)
-C dir       working directory (default: current)
-H host:port Ollama address (default: 127.0.0.1:11434)
```

### Tips
- Bigger model = better edits: `-m qwen2.5-coder:14b` on 16 GB+ machines.
- The agent is allowed to **modify files and run commands**. Review its tool
  calls (printed as `▶ … ◀ …`) and keep work in git.

---

## 中文

### 前提条件
- macOS + 正在运行的 [Ollama](https://ollama.com)（`ollama serve` 或菜单栏应用）
- 已拉取一个对话模型，例如 `ollama pull qwen2.5-coder:3b`
- 编译（clang）：

```bash
make                        # 生成 ./predator_cli
make test                   # 代理单元测试（mock 服务器 e2e）
make install PREFIX=~/.local/bin   # 可选：之后在任意目录运行 `predator_cli`
```

### 单次模式
```bash
./predator_cli "解释 src/ai/agent.c 的作用"
./predator_cli -C /path/to/project "修复 foo.c 的编译警告"
./predator_cli -m qwen2.5-coder:3b -H 127.0.0.1:11434 "给 main() 加上错误处理"
```

### 交互模式（REPL）
```bash
./predator_cli
❯ /model                       # 已安装模型列表
❯ /model 2                     # 按名称或序号切换
❯ /cwd /path/to/project       # 切换工作目录
❯ /reset                      # 清空对话记忆
❯ /stat                       # 查看上下文长度
❯ /quit
```
非 `/` 开头的输入都会发给模型。对话记忆会一直保留，直到 `/reset`。

### 代理能做什么
它像 Claude Code 一样运行工具循环，包含四个限定在工作目录内的工具：

| 工具 | 作用 |
|---|---|
| `list_dir` | 列出目录 |
| `read_file` | 读取文件（上限 8 KB） |
| `edit_file` | 把文件中的 `old_string` 替换为 `new_string` |
| `run_shell` | 执行 shell 命令（30 秒超时，输出截断 4 KB） |

越出工作目录的路径会被拒绝。大约 10 轮工具调用后会强制模型给出答案。

### 参数
```
-m model     对话模型（默认 qwen2.5-coder:3b）
-C dir       工作目录（默认当前目录）
-H host:port Ollama 地址（默认 127.0.0.1:11434）
```

### 提示
- 模型越大编辑质量越好：16 GB 以上内存可用 `-m qwen2.5-coder:14b`。
- 代理**会修改文件并执行命令**。注意查看工具调用输出（`▶ … ◀ …`），并保持代码在 git 管理下。

---

## 한국어

### 요건
- macOS + 실행 중인 [Ollama](https://ollama.com) (`ollama serve` 또는 메뉴바 앱)
- 대화 가능 모델 풀: `ollama pull qwen2.5-coder:3b`
- 빌드 (clang):

```bash
make                        # ./predator_cli 빌드
make test                   # 에이전트 단위 테스트 (목 서버 e2e)
make install PREFIX=~/.local/bin   # 선택: 이후 어디서든 `predator_cli` 실행
```

### 원샷 모드
```bash
./predator_cli "src/ai/agent.c이 어떤 일을 하는지 설명해줘"
./predator_cli -C /path/to/project "foo.c의 컴파일 경고 고쳐줘"
./predator_cli -m qwen2.5-coder:3b -H 127.0.0.1:11434 "main()에 에러 처리 추가해줘"
```

### 대화형 REPL
```bash
./predator_cli
❯ /model                       # 설치된 모델 목록
❯ /model 2                     # 이름 또는 번호로 변경
❯ /cwd /path/to/project       # 작업 디렉터리 변경
❯ /reset                      # 대화 기억 초기화
❯ /stat                       # 대화 길이 확인
❯ /quit
```
`/`로 시작하지 않는 입력은 모두 모델로 전달됩니다. 대화 기억은 `/reset` 전까지 유지됩니다.

### 에이전트가 하는 일
Claude Code처럼 도구 루프를 돌립니다. 작업 디렉터리로 샌드박스된 도구 4개:

| 도구 | 동작 |
|---|---|
| `list_dir` | 디렉터리 나열 |
| `read_file` | 파일 읽기 (8 KB 제한) |
| `edit_file` | 파일에서 `old_string` → `new_string` 치환 |
| `run_shell` | 셸 명령 실행 (30초 타임아웃, 출력 4KB 제한) |

작업 디렉터리를 벗어나는 경로는 거부됩니다. 도구 라운드 약 10회 후에는
가진 정보로 답변하도록 강제됩니다.

### 옵션
```
-m model     대화 모델 (기본 qwen2.5-coder:3b)
-C dir       작업 디렉터리 (기본 현재 디렉터리)
-H host:port Ollama 주소 (기본 127.0.0.1:11434)
```

### 팁
- 모델이 클수록 수정 품질이 좋습니다: 16GB+ 머신에서 `-m qwen2.5-coder:14b`.
- 에이전트는 **파일을 수정하고 명령을 실행**합니다. 도구 호출 출력(`● … ⎿ …`)을
  확인하고, 작업물은 git으로 관리하세요.
