CC      = cc
INC     = -Isrc/ai
CFLAGS  = -std=c11 -Wall -Wextra -Werror -O2 $(INC)

all: predator_cli test

predator_cli: src/ai/agent.c src/cli/main.c
	$(CC) $(CFLAGS) $^ -lcurl -o $@

# system-wide install: run `predator_cli` from anywhere
# (cp to a temp name + mv: overwriting a signed binary in place makes
#  macOS kill it with SIGKILL on exec — a rename gets a fresh inode)
install: predator_cli
	mkdir -p $(PREFIX)
	cp predator_cli $(PREFIX)/.predator_cli.tmp
	mv -f $(PREFIX)/.predator_cli.tmp $(PREFIX)/predator_cli

test: run_agent
	@echo "=== all tests passed ==="

run_agent: src/ai/agent.c tests/test_agent.c
	$(CC) $(CFLAGS) $^ -lcurl -o /tmp/$@
	/tmp/run_agent   # spawns its own mock (tests/mock_ollama_chat.py, port 11436)

run_cli: predator_cli
	python3 tests/mock_ollama_chat.py 11436 >/dev/null 2>&1 & MOCK=$$!; \
	for i in $$(seq 1 40); do curl -s -o /dev/null -X POST -d '{"model":"m","messages":[],"tools":[],"stream":true}' localhost:11436/api/chat && break; sleep 0.5; done; \
	d=$$(mktemp -d); \
	out=$$(./predator_cli -H 127.0.0.1:11436 -C $$d "tests/mock_ollama_chat.py 스크립트대로 작업 수행" 2>/dev/null); RC=$$?; \
	kill $$MOCK 2>/dev/null; \
	echo "$$out" | grep -q "Done: created.txt" || { echo "cli e2e: final answer missing"; rm -rf $$d; exit 1; }; \
	grep -q "hello agent" $$d/created.txt || { echo "cli e2e: wrong file content"; rm -rf $$d; exit 1; }; \
	rm -rf $$d; echo "cli e2e passed"

ci: test run_cli

.PHONY: all install test run_agent run_cli ci
