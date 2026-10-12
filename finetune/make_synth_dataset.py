#!/usr/bin/env python3
"""Synthesize FIM training data (Python / HTML / C) for LVC editor fine-tuning.

Output: MLX-LM chat-less jsonl {"prompt": fim_prompt, "completion": middle}
FIM format: <|fim_prefix|>P<|fim_suffix|>S<|fim_middle|>  ->  M
"""
import json, random, sys

random.seed(42)

# --- corpus of idiomatic snippets per language ---
PY = [
    ("def add(a, b):", "\n    return a + b\n", "\n\ndef sub(a, b):\n    return a - b\n"),
    ("def find_max(xs):", "\n    best = xs[0]\n    for x in xs:\n        if x > best:\n            best = x\n    return best\n", ""),
    ("for i, item in enumerate(items):", "\n        print(i, item)\n", ""),
    ("try:\n    f = open(path)\nexcept IOError as e:", "\n    print('error:', e)\n", ""),
    ("class Stack:\n    def __init__(self):", "\n        self._items = []\n\n    def push(self, x):\n        self._items.append(x)\n", ""),
    ("result = [x * 2 for x in nums if x > 0]", "\nprint(result)\n", ""),
    ("def read_json(path):\n    import json", "\n    with open(path) as f:\n        return json.load(f)\n", ""),
    ("if __name__ == '__main__':", "\n    main()\n", ""),
    ("def fib(n):\n    if n < 2:", "\n        return n\n    return fib(n - 1) + fib(n - 2)\n", ""),
    ("import os\n\nfiles =", " [f for f in os.listdir('.') if f.endswith('.py')]\nprint(len(files))\n", ""),
    ("d = {}\nfor k, v in pairs.items():", "\n    d[k.lower()] = v\nprint(d)\n", ""),
    ("def clamp(x, lo, hi):", "\n    return max(lo, min(hi, x))\n", ""),
    ("with open('out.txt', 'w') as f:", "\n    f.write('hello\\n')\n", ""),
    ("total = sum(x.price for x in cart)", "\nprint(f'total: {total}')\n", ""),
    ("def merge(a, b):\n    out = []\n    i = j = 0\n    while i < len(a) and j < len(b):", "\n        if a[i] < b[j]:\n            out.append(a[i]); i += 1\n        else:\n            out.append(b[j]); j += 1\n    return out + a[i:] + b[j:]\n", ""),
]

HTML = [
    ("<div class=\"card\">", "\n  <h2>Title</h2>\n  <p>Body text</p>\n", "</div>\n"),
    ("<ul class=\"menu\">", "\n  <li><a href=\"/\">Home</a></li>\n  <li><a href=\"/about\">About</a></li>\n", "</ul>\n"),
    ("<table>\n  <thead>\n    <tr>", "<th>Name</th><th>Value</th>", "</tr>\n  </thead>\n</table>\n"),
    ("<form action=\"/submit\" method=\"post\">", "\n  <input type=\"text\" name=\"q\" required>\n  <button type=\"submit\">Go</button>\n", "</form>\n"),
    ("<img src=\"photo.jpg\" alt=\"A photo\"", " width=\"640\" height=\"480\">", "\n"),
    ("<a href=\"https://example.com\"", " target=\"_blank\" rel=\"noopener\">Example</a>", "\n"),
    ("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n  <meta charset=\"utf-8\">", "\n  <title>Page</title>\n", "</head>\n<body></body>\n</html>\n"),
    ("<style>\n  .btn {", "\n    padding: 8px 16px;\n    border-radius: 4px;\n    background: #06f;\n    color: #fff;\n  }\n", "</style>\n"),
    ("<video controls", " poster=\"cover.jpg\">\n  <source src=\"movie.mp4\" type=\"video/mp4\">\n</video>\n", "\n"),
    ("<script>\n  document.querySelector('#go')", "\n    .addEventListener('click', () => alert('hi'));\n", "</script>\n"),
    ("<input type=\"email\"", " name=\"email\" placeholder=\"you@example.com\">", "\n"),
    ("<section id=\"main\">\n  <header>", "<h1>Welcome</h1>", "</header>\n</section>\n"),
]

C = [
    ("int add(int a, int b) {", "\n    return a + b;\n", "}\n"),
    ("for (int i = 0; i < n; i++) {", "\n        sum += a[i];\n    ", "}\n"),
    ("char *s = malloc(len + 1);", "\n    if (!s) return NULL;\n    memcpy(s, src, len);\n    s[len] = '\\0';\n", "\n"),
    ("FILE *f = fopen(path, \"rb\");", "\n    if (!f) return -1;\n    fread(buf, 1, sizeof buf, f);\n    fclose(f);\n", "\n"),
    ("struct point {", "\n    int x, y;\n", "};\n"),
    ("while (*p) {", "\n        if (*p == '\\n') break;\n        p++;\n    ", "}\n"),
    ("void swap(int *a, int *b) {", "\n    int t = *a; *a = *b; *b = t;\n", "}\n"),
    ("if (p == NULL) {", "\n        fprintf(stderr, \"out of memory\\n\");\n        return -1;\n    ", "}\n"),
    ("static inline uint32_t hash32(uint32_t x) {", "\n    x ^= x >> 16; x *= 0x7feb352dU;\n    x ^= x >> 15; x *= 0x846ca68bU;\n    return x ^ (x >> 16);\n", "}\n"),
    ("int cmp(const void *a, const void *b) {", "\n    return *(const int *)a - *(const int *)b;\n", "}\n"),
    ("qsort(arr, n, sizeof(int), cmp);", "\nfor (int i = 0; i < n; i++) printf(\"%d\\n\", arr[i]);\n", "\n"),
    ("size_t len = strlen(src);", "\nchar *dst = strdup(src);\nprintf(\"%zu\\n\", len);\n", "\n"),
    ("pthread_mutex_lock(&m);", "\ncount++;\npthread_mutex_unlock(&m);\n", "\n"),
    ("#include <stdio.h>\n\nint main(void) {", "\n    printf(\"hello\\n\");\n    return 0;\n", "}\n"),
    ("double vec_dot(const float *a, const float *b, int n) {", "\n    double s = 0;\n    for (int i = 0; i < n; i++) s += a[i] * b[i];\n    return s;\n", "}\n"),
]


def fim_example(prefix, middle, suffix):
    return {
        "prompt": f"<|fim_prefix|>{prefix}<|fim_suffix|>{suffix}<|fim_middle|>",
        "completion": middle,
    }


def main(out_path, target=1200):
    rows = []
    pools = {"python": PY, "html": HTML, "c": C}
    per = target // 3
    for lang, pool in pools.items():
        for i in range(per):
            prefix, middle, suffix = random.choice(pool)
            # randomly trim prefix/suffix to vary cursor position
            if random.random() < 0.4:
                cut = max(1, len(prefix) // 2)
                prefix = prefix[:cut]
            if suffix and random.random() < 0.3:
                suffix = ""
            rows.append(fim_example(prefix, middle, suffix))
    random.shuffle(rows)
    with open(out_path, "w") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"wrote {len(rows)} examples -> {out_path}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "fim_dataset.jsonl")
