#!/usr/bin/env python3
"""QLoRA fine-tune a small FIM coder model with Unsloth, export merged GGUF.

Target: MacBook Neo (A18 Pro, 8GB) — base model must be <=3B.
Base recommendation: unsloth/Qwen2.5-Coder-1.5B-bnb-4bit (FIM-capable family).

Usage:
  python3 train_fim.py --dataset fim_dataset.jsonl --base unsloth/Qwen2.5-Coder-1.5B-bnb-4bit
  # output: ./out/model-q4_k_m.gguf ready for `ollama create`
"""
import argparse, os, subprocess, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--base", default="unsloth/Qwen2.5-Coder-1.5B-bnb-4bit")
    ap.add_argument("--out", default="out")
    ap.add_argument("--epochs", type=float, default=3.0)
    ap.add_argument("--bs", type=int, default=2)
    ap.add_argument("--grad-accum", type=int, default=4)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--lora-r", type=int, default=16)
    ap.add_argument("--quant", default="Q4_K_M")
    args = ap.parse_args()

    try:
        from unsloth import FastLanguageModel  # noqa
    except ImportError:
        sys.exit("unsloth not installed. Run: pip install unsloth")

    from unsloth import FastLanguageModel
    from datasets import load_dataset
    from trl import SFTTrainer
    from transformers import TrainingArguments

    os.makedirs(args.out, exist_ok=True)

    model, tokenizer = FastLanguageModel.from_pretrained(
        model_name=args.base, max_seq_length=2048, load_in_4bit=True)
    model = FastLanguageModel.get_peft_model(
        model, r=args.lora_r, lora_alpha=args.lora_r * 2, lora_dropout=0,
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj",
                        "gate_proj", "up_proj", "down_proj"],
        use_gradient_checkpointing="unsloth")

    ds = load_dataset("json", data_files=args.dataset, split="train")
    def fmt(ex):
        return {"text": ex["prompt"] + ex["completion"] + tokenizer.eos_token}
    ds = ds.map(fmt)

    trainer = SFTTrainer(
        model=model, tokenizer=tokenizer, train_dataset=ds,
        dataset_text_field="text", max_seq_length=2048,
        args=TrainingArguments(
            per_device_train_batch_size=args.bs,
            gradient_accumulation_steps=args.grad_accum,
            num_train_epochs=args.epochs,
            learning_rate=args.lr, fp16=not os.uname().machine.startswith("arm"),
            bf16=os.uname().machine.startswith("arm") and sys.platform == "linux",
            logging_steps=5, output_dir=args.out, save_strategy="no"))

    trainer.train()
    model.save_pretrained_merged(os.path.join(args.out, "merged"), tokenizer,
                                 save_method="merged_16bit")

    # merge -> GGUF -> quantize
    merged = os.path.join(args.out, "merged")
    f16 = os.path.join(args.out, "model-f16.gguf")
    q = os.path.join(args.out, "model-q4_k_m.gguf")
    llama_cpp = os.environ.get("LLAMA_CPP_DIR", "vendor/llama.cpp")
    subprocess.run([sys.executable, os.path.join(llama_cpp, "convert_hf_to_gguf.py"),
                    merged, "--outfile", f16, "--outtype", "f16"], check=True)
    subprocess.run([os.path.join(llama_cpp, "build", "bin", "llama-quantize"),
                    f16, q, args.quant], check=True)
    print(f"done -> {q}")

if __name__ == "__main__":
    main()
