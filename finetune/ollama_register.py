#!/usr/bin/env python3
"""Register a fine-tuned GGUF into local Ollama (Modelfile + ollama create).

Also supports a base+LoRA-GGUF merge path via llama-export-lora.

Usage:
  python3 ollama_register.py --gguf out/model-q4_k_m.gguf --name my-coder
  python3 ollama_register.py --base base.gguf --lora adapter.gguf --name my-coder
"""
import argparse, os, subprocess, sys

TEMPLATE = """TEMPLATE \"\"\"{{ if .System }}{{ .System }}{{ end }}{{ .Prompt }}\"\"\"
PARAMETER stop <|fim_middle|>
PARAMETER stop <||end|>
PARAMETER temperature 0.2
PARAMETER num_predict 128
"""

def sh(cmd, **kw):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, **kw)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gguf", help="merged model gguf")
    ap.add_argument("--base", help="base gguf (with --lora)")
    ap.add_argument("--lora", help="lora adapter gguf (with --base)")
    ap.add_argument("--llama-cpp", default="vendor/llama.cpp",
                    help="llama.cpp dir for llama-export-lora merge path")
    ap.add_argument("--name", required=True)
    ap.add_argument("--dir", default="out")
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    if args.base and args.lora:
        merged = os.path.join(args.dir, f"{args.name}-merged.gguf")
        tool = os.path.join(args.llama_cpp, "build", "bin", "llama-export-lora")
        r = sh([tool, "-m", args.base, "-o", merged, "--lora", args.lora])
        if r.returncode != 0:
            sys.exit("llama-export-lora failed — build llama.cpp first")
        gguf = merged
    elif args.gguf:
        gguf = args.gguf
    else:
        sys.exit("pass --gguf or --base/--lora")

    modelfile = os.path.join(args.dir, "Modelfile")
    with open(modelfile, "w") as f:
        f.write(f"FROM {os.path.abspath(gguf)}\n{TEMPLATE}")
    r = sh(["ollama", "create", args.name, "-f", modelfile])
    if r.returncode != 0:
        sys.exit("ollama create failed — is Ollama running?")
    print(f"registered as '{args.name}'. test: ollama run {args.name}")

if __name__ == "__main__":
    main()
