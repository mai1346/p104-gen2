#!/usr/bin/env python3
"""lora_gguf_remap.py —— 修复 int8_convrot 底模 LoRA × GGUF 模型的键名错配

问题：
  在 qwen_image_2.1_int8_convrot 底模上训练的 LoRA，其 MLP 键为融合命名：
      diffusion_model.transformer_blocks.N.img_mlp.gate_up.lora_A/B.weight
  而 GGUF 量化模型（UC-Q4_K_M 等）的同一层是拆开的：
      diffusion_model.transformer_blocks.N.img_mlp.gate_layer.lora_A/B.weight
      diffusion_model.transformer_blocks.N.img_mlp.proj.lora_A/B.weight
  直接把这种 LoRA 挂到 GGUF 模型上，gate_up 键全部报
  "lora key not loaded"（每块 2 条 × 32 层 = 64 条），LoRA 只有部分效果。

修复（顺序依据 comfy/ldm/qwen_image21/model.py）：
  fused gate_up = [gate; up] 一次 GEMM
  unfused forward = out(silu(gate_layer(x)) * proj(x))
  ⇒ lora_A 共享给 gate_layer 和 proj；
    lora_B 按行切开：前半 → gate_layer，后半 → proj。

用法:
  python3 lora_gguf_remap.py <输入.safetensors> [输出.safetensors]
  输出缺省为 <输入名>_ggufcompat.safetensors（同目录）

验证:
  装入 ComfyUI 跑一次生成，日志中 "lora key not loaded" 不应新增。
"""
import sys
import os
from safetensors.torch import load_file, save_file

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.splitext(src)[0] + "_ggufcompat.safetensors"

    sd = load_file(src)
    out = {}
    split_cnt = 0
    removed = 0

    for k, t in sd.items():
        if ".img_mlp.gate_up.lora_A.weight" in k:
            base = k.replace(".img_mlp.gate_up.lora_A.weight", "")
            out[f"{base}.img_mlp.gate_layer.lora_A.weight"] = t.clone()
            out[f"{base}.img_mlp.proj.lora_A.weight"] = t.clone()
            split_cnt += 1
        elif ".img_mlp.gate_up.lora_B.weight" in k:
            base = k.replace(".img_mlp.gate_up.lora_B.weight", "")
            h = t.shape[0] // 2          # [2*FF, r] → [gate FF; up FF]
            out[f"{base}.img_mlp.gate_layer.lora_B.weight"] = t[:h].contiguous()
            out[f"{base}.img_mlp.proj.lora_B.weight"] = t[h:].contiguous()
            removed += 2
        else:
            out[k] = t

    save_file(out, dst)
    print(f"输入张量: {len(sd)}")
    print(f"gate_up 拆分块: {split_cnt}（每块 1→2 张量，删 2 旧键）")
    print(f"输出张量: {len(out)}（应为 输入 - {removed} + {split_cnt*2}）")
    print(f"已保存: {dst}")

if __name__ == "__main__":
    main()
