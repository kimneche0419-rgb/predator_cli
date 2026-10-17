CC      = cc
INC     = -Isrc/ai
CFLAGS  = -std=c11 -Wall -Wextra -Werror -O2 $(INC)

all: lvcli test

lvcli: src/ai/agent.c src/cli/main.c
	$(CC) $(CFLAGS) $^ -lcurl -o $@

test: run_agent
	@echo "=== all tests passed ==="

run_agent: src/ai/agent.c tests/test_agent.c
	$(CC) $(CFLAGS) $^ -lcurl -o /tmp/$@
	python3 tests/mock_ollama_chat.py 11436 >/dev/null 2>&1 & MOCK=$$!; \
	for i in $$(seq 1 40); do curl -s -o /dev/null -X POST -d '{"model":"m","messages":[],"tools":[],"stream":true}' localhost:11436/api/chat && break; sleep 0.5; done; \
	/tmp/run_agent; RC=$$?; kill $$MOCK 2>/dev/null; exit $$RC

run_cli: lvcli
	python3 tests/mock_ollama_chat.py 11436 >/dev/null 2>&1 & MOCK=$$!; \
	for i in $$(seq 1 40); do curl -s -o /dev/null -X POST -d '{"model":"m","messages":[],"tools":[],"stream":true}' localhost:11436/api/chat && break; sleep 0.5; done; \
	d=$$(mktemp -d); \
	out=$$(./lvcli -H 127.0.0.1:11436 -C $$d "작업" 2>/dev/null); RC=$$?; \
	kill $$MOCK 2>/dev/null; \
	echo "$$out" | grep -q "Done: created.txt" || { echo "cli e2e: final answer missing"; rm -rf $$d; exit 1; }; \
	grep -q "hello agent" $$d/created.txt || { echo "cli e2e: wrong file content"; rm -rf $$d; exit 1; }; \
	rm -rf $$d; echo "cli e2e passed"

ci: test run_cli

.PHONY: all test run_agent run_cli ci
