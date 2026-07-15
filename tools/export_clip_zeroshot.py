"""Export CLIP ViT-B/32 as a zero-shot 5-class scene classifier ONNX model.

Stage A of AetherFlow Phase 4 P0.1 — see `docs/archive/PHASE4_P0_PLAN.md` §2.2.A, §8.

Output: `models/scene_classifier_v1.onnx`
  input:  image  [1, 3, 224, 224] float32, ImageNet-normalized
          (mean=[0.48145466, 0.4578275, 0.40821073],
           std =[0.26862954, 0.26130258, 0.27577711])  # CLIP-specific
  output: logits [1, 5] float32 (cosine similarity * 100, NO softmax applied)

Class index mapping (canonical, must match runtime + verifier):
  0 -> code_text          1 -> slides    2 -> video
  3 -> mixed_ui           4 -> sensitive_surface

Determinism: pinned open_clip ViT-B-32 / openai checkpoint, torch.manual_seed(0).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import open_clip
import torch
import torch.nn as nn


PROMPTS = [
    "a screenshot of code or a terminal window",            # 0 code_text
    "a screenshot of presentation slides",                  # 1 slides
    "a screenshot of a video playing",                      # 2 video
    "a screenshot of a web browser with mixed UI",          # 3 mixed_ui
    "a screenshot containing a notification, chat window, "
    "or sensitive UI",                                      # 4 sensitive_surface
]
CLASS_NAMES = ["code_text", "slides", "video", "mixed_ui", "sensitive_surface"]


class ZeroShotClassifier(nn.Module):
    """CLIP visual encoder + frozen text-embedding head.

    Computes cosine similarity between the image embedding and the
    five pre-computed text-prompt embeddings, scaled by 100. The
    runtime applies softmax to get per-class confidence.

    Holds only the visual encoder (text encoder is dropped after the
    prompt embeddings are precomputed) to keep the exported .onnx
    file small.
    """

    def __init__(self, visual_encoder: nn.Module, text_features: torch.Tensor):
        super().__init__()
        self.visual = visual_encoder
        self.register_buffer("text_features", text_features)

    def forward(self, image: torch.Tensor) -> torch.Tensor:
        image_features = self.visual(image)
        image_features = image_features / image_features.norm(dim=-1, keepdim=True)
        return (image_features @ self.text_features.T) * 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", default="models/scene_classifier_v1.onnx",
                        help="Output ONNX path (default: models/scene_classifier_v1.onnx)")
    parser.add_argument("--opset", type=int, default=17)
    args = parser.parse_args()

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    torch.manual_seed(0)
    print(f"[export] loading open_clip ViT-B-32 (openai pretrained, QuickGELU)")
    model, _, _ = open_clip.create_model_and_transforms(
        "ViT-B-32", pretrained="openai", force_quick_gelu=True
    )
    tokenizer = open_clip.get_tokenizer("ViT-B-32")
    model.eval()

    print(f"[export] tokenizing {len(PROMPTS)} prompts, freezing text features")
    with torch.no_grad():
        tokens = tokenizer(PROMPTS)
        text_features = model.encode_text(tokens)
        text_features = text_features / text_features.norm(dim=-1, keepdim=True)

    # Keep only the visual encoder for export — text encoder weights are
    # baked into `text_features` and no longer need to ship in the .onnx.
    wrapper = ZeroShotClassifier(model.visual, text_features).eval()

    # Sanity-run torch forward before export
    dummy = torch.zeros(1, 3, 224, 224)
    with torch.no_grad():
        torch_out = wrapper(dummy)
    assert torch_out.shape == (1, 5), f"unexpected torch output shape {torch_out.shape}"

    print(f"[export] torch.onnx.export -> {output_path} (opset={args.opset})")
    torch.onnx.export(
        wrapper,
        dummy,
        str(output_path),
        input_names=["image"],
        output_names=["logits"],
        opset_version=args.opset,
        do_constant_folding=True,
        dynamo=False,
    )

    print(f"[export] embedding metadata")
    m = onnx.load(str(output_path))
    m.producer_name = "aetherflow-export-clip-zeroshot"
    m.producer_version = "stage-a-v1"
    m.model_version = 1
    m.doc_string = (
        f"CLIP ViT-B/32 zero-shot, 5 classes [{', '.join(CLASS_NAMES)}], Stage A. "
        f"open_clip={open_clip.__version__}, torch={torch.__version__}, opset={args.opset}."
    )
    onnx.checker.check_model(m)
    onnx.save(m, str(output_path))

    print(f"[verify] onnxruntime CPU EP smoke")
    sess = ort.InferenceSession(str(output_path), providers=["CPUExecutionProvider"])
    rand_input = np.random.RandomState(0).randn(1, 3, 224, 224).astype(np.float32)
    logits = sess.run(None, {"image": rand_input})[0]
    assert logits.shape == (1, 5), f"unexpected onnx output shape {logits.shape}"
    assert not np.isnan(logits).any(), "NaN in onnx output"
    print(f"[verify] OK   shape={logits.shape}   sample logits={logits[0]}")

    file_mb = output_path.stat().st_size / (1024 * 1024)
    print(f"[done] {output_path}  ({file_mb:.1f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
