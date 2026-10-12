CC = cc
TS_INC = -Ivendor/tree-sitter/lib/include -Ivendor/tree-sitter/lib/src -Ivendor/tree-sitter-c/src
INC = $(TS_INC) -Isrc/core -Isrc/ai
CORE = src/core/buffer.c src/core/io.c src/core/undo.c src/core/syntax.c src/core/diff.c src/ai/fim.c src/ai/ollama.c src/ai/lsp.c src/ai/agent.c
TS = vendor/tree-sitter-c/src/parser.c vendor/tree-sitter/lib/src/lib.c
LD = -framework AppKit -framework CoreText -lcurl

all: lvc app lvcli test

lvc: $(CORE) $(TS) src/ui/shell.m src/ui/metal_view.m
	$(CC) -ObjC -O2 -Wall $(INC) $(CORE) $(TS) src/ui/shell.m src/ui/metal_view.m \
		-framework AppKit -framework CoreText -framework Metal -framework MetalKit -lcurl -o $@

app: lvc
	cp lvc LVC.app/Contents/MacOS/lvc

test: run_buffer run_io_undo run_syntax run_fim run_ollama run_agent
	@echo "=== all tests passed ==="

run_buffer: src/core/buffer.c tests/test_buffer.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $(INC) $^ -lcurl -o /tmp/$@
	/tmp/$@

run_io_undo: src/core/buffer.c src/core/io.c src/core/undo.c tests/test_io_undo.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $(INC) $^ -lcurl -o /tmp/$@
	/tmp/$@

run_syntax: $(CORE) $(TS) tests/test_syntax.c
	$(CC) -std=c11 -Wall -O2 $(INC) $^ $(LD) -o /tmp/$@
	/tmp/$@

run_fim: src/core/buffer.c src/ai/fim.c src/ai/ollama.c tests/test_fim.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $(INC) $^ -lcurl -o /tmp/$@
	/tmp/$@

run_ollama: src/ai/ollama.c tests/test_ollama.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $(INC) $^ -lcurl -o /tmp/run_ollama
	python3 tests/mock_ollama.py 11435 >/dev/null 2>&1 & MOCK=$$!; sleep 1; \
	/tmp/run_ollama; RC=$$?; kill $$MOCK 2>/dev/null; exit $$RC

run_agent: src/ai/agent.c tests/test_agent.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $(INC) $^ -lcurl -o /tmp/$@
	python3 tests/mock_ollama_chat.py 11436 >/dev/null 2>&1 & MOCK=$$!; \
	for i in $$(seq 1 40); do curl -s -o /dev/null -X POST -d '{"model":"m","messages":[],"tools":[],"stream":true}' localhost:11436/api/chat && break; sleep 0.5; done; \
	/tmp/run_agent; RC=$$?; kill $$MOCK 2>/dev/null; exit $$RC

run_fuzz: src/core/buffer.c src/core/io.c src/core/undo.c tests/test_fuzz.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $(INC) $^ -lcurl -o /tmp/$@
	/tmp/$@

run_asan:
	$(CC) -std=c11 -Wall -Wextra -O2 -fsanitize=address,undefined -g $(INC) src/core/buffer.c tests/test_buffer.c -lcurl -o /tmp/asan_buffer
	/tmp/asan_buffer
	$(CC) -std=c11 -Wall -Wextra -O2 -fsanitize=address,undefined -g $(INC) src/core/buffer.c src/core/io.c src/core/undo.c tests/test_io_undo.c -lcurl -o /tmp/asan_io_undo
	/tmp/asan_io_undo
	$(CC) -std=c11 -Wall -Wextra -O2 -fsanitize=address,undefined -g $(INC) src/core/buffer.c src/core/io.c src/core/undo.c tests/test_fuzz.c -lcurl -o /tmp/asan_fuzz
	/tmp/asan_fuzz

lvcli: src/ai/agent.c src/cli/main.c
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $(INC) $^ -lcurl -o $@

run_cli: lvcli
	python3 tests/mock_ollama_chat.py 11436 >/dev/null 2>&1 & MOCK=$$!; \
	for i in $$(seq 1 40); do curl -s -o /dev/null -X POST -d '{"model":"m","messages":[],"tools":[],"stream":true}' localhost:11436/api/chat && break; sleep 0.5; done; \
	d=$$(mktemp -d); \
	out=$$(./lvcli -H 127.0.0.1:11436 -C $$d "작업" 2>/dev/null); RC=$$?; \
	kill $$MOCK 2>/dev/null; \
	echo "$$out" | grep -q "Done: created.txt" || { echo "cli e2e: final answer missing"; rm -rf $$d; exit 1; }; \
	grep -q "hello agent" $$d/created.txt || { echo "cli e2e: wrong file content"; rm -rf $$d; exit 1; }; \
	rm -rf $$d; echo "cli e2e passed"

ci: test run_fuzz run_asan run_fim run_ollama run_agent run_cli

accept-report:
	python3 scripts/accept_report.py
