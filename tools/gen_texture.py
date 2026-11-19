#!/usr/bin/env python3
"""
Texture generator for voxelforge.

Takes a texture description and generates a high-resolution, seamless,
photorealistic texture using ComfyUI's local API.

Usage:
    python3 gen_texture.py "mossy cobblestone, weathered, green moss patches"
    python3 gen_texture.py "oak wood planks, aged, warm brown" --size 1024 --count 4
    python3 gen_texture.py "fine sand, beige, granular" --out assets/textures/sand.png

Requirements:
    - ComfyUI running at http://127.0.0.1:8188 (start with: cd ~/comfyui && python main.py)
    - A checkpoint model in ~/comfyui/models/checkpoints/ (e.g. Juggernaut-XL-v9.safetensors)
"""

import argparse
import json
import os
import sys
import time
import urllib.request
import urllib.parse
import uuid

COMFYUI_URL = "http://127.0.0.1:8188"
DEFAULT_CHECKPOINT = "Juggernaut-XL-v9.safetensors"
DEFAULT_SIZE = 1024
DEFAULT_STEPS = 30
DEFAULT_CFG = 7.0
DEFAULT_NEGATIVE = (
    "text, watermark, logo, signature, border, frame, "
    "shadow, highlight, vignette, gradient, "
    "blurry, low quality, jpeg artifacts, "
    "person, animal, object, scene, landscape, "
    "non-seamless, visible seam, tile edge"
)

TEXTURE_PROMPT_TEMPLATE = (
    "seamless tileable texture, {desc}, "
    "photorealistic, high resolution, flat even lighting, "
    "no shadows, no highlights, diffuse albedo only, "
    "surface material, top-down view, "
    "clean uniform illumination, PBR material"
)


def build_workflow(prompt, negative, width, height, steps, cfg, seed, checkpoint):
    """Build a ComfyUI workflow graph for texture generation."""
    positive = TEXTURE_PROMPT_TEMPLATE.format(desc=prompt)

    workflow = {
        "3": {
            "class_type": "KSampler",
            "inputs": {
                "seed": seed,
                "steps": steps,
                "cfg": cfg,
                "sampler_name": "dpmpp_2m",
                "scheduler": "karras",
                "denoise": 1.0,
                "model": ["4", 0],
                "positive": ["6", 0],
                "negative": ["7", 0],
                "latent_image": ["5", 0],
            },
        },
        "4": {
            "class_type": "CheckpointLoaderSimple",
            "inputs": {"ckpt_name": checkpoint},
        },
        "5": {
            "class_type": "EmptyLatentImage",
            "inputs": {"width": width, "height": height, "batch_size": 1},
        },
        "6": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": positive, "clip": ["4", 1]},
        },
        "7": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": negative, "clip": ["4", 1]},
        },
        "8": {
            "class_type": "VAEDecode",
            "inputs": {"samples": ["3", 0], "vae": ["4", 2]},
        },
        "9": {
            "class_type": "SaveImage",
            "inputs": {
                "filename_prefix": "texture",
                "images": ["8", 0],
            },
        },
    }
    return workflow


def queue_prompt(workflow):
    """Send workflow to ComfyUI and return prompt_id."""
    data = json.dumps({"prompt": workflow}).encode("utf-8")
    req = urllib.request.Request(
        f"{COMFYUI_URL}/prompt",
        data=data,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req) as resp:
        result = json.loads(resp.read())
    return result["prompt_id"]


def wait_for_completion(prompt_id, timeout=300):
    """Poll ComfyUI history until the prompt is done."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            with urllib.request.urlopen(f"{COMFYUI_URL}/history/{prompt_id}") as resp:
                history = json.loads(resp.read())
            if prompt_id in history:
                return history[prompt_id]
        except Exception:
            pass
        time.sleep(2)
    raise TimeoutError(f"Generation timed out after {timeout}s")


def get_output_images(history):
    """Extract output image filenames from history."""
    images = []
    for node_id, node_output in history.get("outputs", {}).items():
        if "images" in node_output:
            for img in node_output["images"]:
                images.append(img["filename"])
    return images


def download_image(filename, output_path):
    """Download generated image from ComfyUI."""
    params = urllib.parse.urlencode({"filename": filename, "type": "output"})
    url = f"{COMFYUI_URL}/view?{params}"
    urllib.request.urlretrieve(url, output_path)


def check_comfyui():
    """Check if ComfyUI is running."""
    try:
        with urllib.request.urlopen(f"{COMFYUI_URL}/system_stats") as resp:
            return True
    except Exception:
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Generate high-res photorealistic textures via ComfyUI"
    )
    parser.add_argument("description", help="Texture description (e.g. 'mossy cobblestone')")
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE,
                        help=f"Output size in pixels (default: {DEFAULT_SIZE})")
    parser.add_argument("--count", type=int, default=1,
                        help="Number of textures to generate (default: 1)")
    parser.add_argument("--steps", type=int, default=DEFAULT_STEPS,
                        help=f"Sampling steps (default: {DEFAULT_STEPS})")
    parser.add_argument("--cfg", type=float, default=DEFAULT_CFG,
                        help=f"CFG scale (default: {DEFAULT_CFG})")
    parser.add_argument("--seed", type=int, default=None,
                        help="Random seed (default: random)")
    parser.add_argument("--checkpoint", default=DEFAULT_CHECKPOINT,
                        help=f"Checkpoint filename (default: {DEFAULT_CHECKPOINT})")
    parser.add_argument("--negative", default=DEFAULT_NEGATIVE,
                        help="Negative prompt")
    parser.add_argument("--out", default=None,
                        help="Output path (default: assets/textures/<slug>.png)")
    parser.add_argument("--out-dir", default="assets/textures",
                        help="Output directory (default: assets/textures)")
    args = parser.parse_args()

    if not check_comfyui():
        print("ERROR: ComfyUI is not running at", COMFYUI_URL)
        print("Start it with: cd ~/comfyui && python main.py")
        sys.exit(1)

    # Resolve output path
    slug = args.description.lower().replace(" ", "_").replace(",", "")[:50]
    slug = "".join(c for c in slug if c.isalnum() or c == "_")

    os.makedirs(args.out_dir, exist_ok=True)

    for i in range(args.count):
        seed = args.seed if args.seed is not None else (int(time.time() * 1000) + i * 1000) % (2**32)

        if args.out:
            if args.count > 1:
                base, ext = os.path.splitext(args.out)
                out_path = f"{base}_{i+1}{ext}"
            else:
                out_path = args.out
        else:
            out_path = os.path.join(args.out_dir, f"{slug}_{i+1}.png")

        print(f"\n[{i+1}/{args.count}] Generating: {args.description}")
        print(f"  Size: {args.size}x{args.size}, Steps: {args.steps}, Seed: {seed}")
        print(f"  Output: {out_path}")

        workflow = build_workflow(
            prompt=args.description,
            negative=args.negative,
            width=args.size,
            height=args.size,
            steps=args.steps,
            cfg=args.cfg,
            seed=seed,
            checkpoint=args.checkpoint,
        )

        prompt_id = queue_prompt(workflow)
        print(f"  Queued: {prompt_id}")

        history = wait_for_completion(prompt_id)
        images = get_output_images(history)

        if not images:
            print("  ERROR: No output images found")
            continue

        download_image(images[0], out_path)
        print(f"  Saved: {out_path} ({os.path.getsize(out_path) / 1024:.0f} KB)")

    print(f"\nDone! Generated {args.count} texture(s).")
    print("Run tools/check_texture.py to verify conformance before using in voxelforge.")


if __name__ == "__main__":
    main()
