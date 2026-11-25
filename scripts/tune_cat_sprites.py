#!/usr/bin/env python3
"""Manual tuning UI for cat status sprites.

Loads docs/cat_sprites/*_raw.png, lets you tune scale and position for each
state, then writes the final icon PNGs, preview.jpg, tuning.json, and firmware
ARGB8888 .bin assets.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import ttk

from PIL import Image, ImageTk


STATE_NAMES = (
    "pairing",
    "ready",
    "listening",
    "thinking",
    "resting",
    "error",
)

DEFAULT_TUNING = {
    "pairing": {"scale": 1.00, "x": 0, "y": 0},
    "ready": {"scale": 1.00, "x": 0, "y": 0},
    "listening": {"scale": 1.00, "x": 0, "y": 0},
    "thinking": {"scale": 1.00, "x": 0, "y": 0},
    "resting": {"scale": 1.00, "x": 0, "y": 0},
    "error": {"scale": 1.00, "x": 0, "y": 0},
}


@dataclass
class Paths:
    sprite_dir: Path
    assets_dir: Path
    size: int

    @property
    def tuning_path(self) -> Path:
        return self.sprite_dir / "tuning.json"

    @property
    def preview_path(self) -> Path:
        return self.sprite_dir / "preview.jpg"


def raw_path(paths: Paths, index: int, state: str) -> Path:
    return paths.sprite_dir / f"{index + 1}_{state}_raw.png"


def icon_path(paths: Paths, index: int, state: str) -> Path:
    return paths.sprite_dir / f"{index + 1}_{state}_{paths.size}.png"


def checkerboard(size: tuple[int, int], cell: int = 8) -> Image.Image:
    image = Image.new("RGBA", size, (255, 255, 255, 255))
    for y in range(size[1]):
        for x in range(size[0]):
            if ((x // cell) + (y // cell)) % 2:
                image.putpixel((x, y), (232, 232, 232, 255))
    return image


def render_icon(raw: Image.Image, size: int, scale: float, offset_x: int, offset_y: int) -> Image.Image:
    image = raw.convert("RGBA")
    base_scale = min(size / image.width, size / image.height)
    width = max(1, round(image.width * base_scale * scale))
    height = max(1, round(image.height * base_scale * scale))
    image = image.resize((width, height), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(image, ((size - width) // 2 + offset_x, (size - height) // 2 + offset_y))
    return canvas


def load_tuning(paths: Paths) -> dict[str, dict[str, float | int]]:
    tuning = json.loads(json.dumps(DEFAULT_TUNING))
    if paths.tuning_path.exists():
        loaded = json.loads(paths.tuning_path.read_text())
        for state in STATE_NAMES:
            tuning[state].update(loaded.get(state, {}))
    return tuning


def save_tuning(paths: Paths, tuning: dict[str, dict[str, float | int]]) -> None:
    paths.tuning_path.write_text(json.dumps(tuning, indent=2) + "\n")


def write_preview(paths: Paths) -> None:
    tile = paths.size + 32
    preview = checkerboard((tile * 3, tile * 2))
    for index, state in enumerate(STATE_NAMES):
        icon = Image.open(icon_path(paths, index, state)).convert("RGBA")
        x = (index % 3) * tile + (tile - paths.size) // 2
        y = (index // 3) * tile + (tile - paths.size) // 2
        preview.alpha_composite(icon, (x, y))
    preview.convert("RGB").save(paths.preview_path, quality=95)


def convert_bins(paths: Paths) -> None:
    sources = [str(icon_path(paths, index, state)) for index, state in enumerate(STATE_NAMES)]
    subprocess.run(
        [sys.executable, "scripts/png_to_lvgl_argb_bin.py", *sources, "--out-dir", str(paths.assets_dir)],
        check=True,
    )


class SpriteTuner:
    def __init__(self, root: tk.Tk, paths: Paths):
        self.root = root
        self.paths = paths
        self.tuning = load_tuning(paths)
        self.raw_images = [
            Image.open(raw_path(paths, index, state)).convert("RGBA")
            for index, state in enumerate(STATE_NAMES)
        ]
        self.tk_images: list[ImageTk.PhotoImage] = []
        self.image_labels: list[ttk.Label] = []
        self.scale_vars: dict[str, tk.DoubleVar] = {}
        self.x_vars: dict[str, tk.IntVar] = {}
        self.y_vars: dict[str, tk.IntVar] = {}
        self.status_var = tk.StringVar(value="Adjust scale and position, then Save.")
        self.build()
        self.refresh_all()

    def build(self) -> None:
        self.root.title("Cat Sprite Tuner")
        self.root.resizable(False, False)
        main = ttk.Frame(self.root, padding=12)
        main.grid(row=0, column=0, sticky="nsew")

        for index, state in enumerate(STATE_NAMES):
            frame = ttk.LabelFrame(main, text=state, padding=8)
            frame.grid(row=index // 3, column=index % 3, padx=6, pady=6, sticky="n")

            image_label = ttk.Label(frame)
            image_label.grid(row=0, column=0, columnspan=3, pady=(0, 8))
            self.image_labels.append(image_label)

            tuning = self.tuning[state]
            self.scale_vars[state] = tk.DoubleVar(value=float(tuning["scale"]))
            self.x_vars[state] = tk.IntVar(value=int(tuning["x"]))
            self.y_vars[state] = tk.IntVar(value=int(tuning["y"]))

            self.add_control(frame, state, "scale", self.scale_vars[state], 0.65, 1.55, 0.01, 1)
            self.add_control(frame, state, "x", self.x_vars[state], -24, 24, 1, 2)
            self.add_control(frame, state, "y", self.y_vars[state], -24, 24, 1, 3)

        buttons = ttk.Frame(main)
        buttons.grid(row=2, column=0, columnspan=3, sticky="ew", pady=(8, 0))
        ttk.Button(buttons, text="Save final sprites", command=self.save).pack(side="left")
        ttk.Button(buttons, text="Reset current values", command=self.reset).pack(side="left", padx=8)
        ttk.Label(buttons, textvariable=self.status_var).pack(side="left", padx=8)

    def add_control(
        self,
        parent: ttk.Frame,
        state: str,
        label: str,
        var: tk.Variable,
        from_: float,
        to: float,
        resolution: float,
        row: int,
    ) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w")
        scale = tk.Scale(
            parent,
            variable=var,
            from_=from_,
            to=to,
            resolution=resolution,
            orient="horizontal",
            length=130,
            showvalue=True,
            command=lambda _value, state=state: self.refresh_state(state),
        )
        scale.grid(row=row, column=1, sticky="ew")

    def render_state(self, state: str) -> Image.Image:
        index = STATE_NAMES.index(state)
        return render_icon(
            self.raw_images[index],
            self.paths.size,
            self.scale_vars[state].get(),
            self.x_vars[state].get(),
            self.y_vars[state].get(),
        )

    def refresh_state(self, state: str) -> None:
        icon = self.render_state(state)
        preview = checkerboard((self.paths.size, self.paths.size))
        preview.alpha_composite(icon)
        tk_image = ImageTk.PhotoImage(preview)
        index = STATE_NAMES.index(state)
        while len(self.tk_images) <= index:
            self.tk_images.append(tk_image)
        self.tk_images[index] = tk_image
        self.image_labels[index].configure(image=tk_image)

    def refresh_all(self) -> None:
        for state in STATE_NAMES:
            self.refresh_state(state)

    def collect_tuning(self) -> dict[str, dict[str, float | int]]:
        return {
            state: {
                "scale": round(self.scale_vars[state].get(), 3),
                "x": self.x_vars[state].get(),
                "y": self.y_vars[state].get(),
            }
            for state in STATE_NAMES
        }

    def save(self) -> None:
        self.tuning = self.collect_tuning()
        save_tuning(self.paths, self.tuning)
        for index, state in enumerate(STATE_NAMES):
            self.render_state(state).save(icon_path(self.paths, index, state))
        write_preview(self.paths)
        convert_bins(self.paths)
        self.status_var.set(f"Saved {self.paths.preview_path} and firmware assets.")

    def reset(self) -> None:
        for state in STATE_NAMES:
            self.scale_vars[state].set(DEFAULT_TUNING[state]["scale"])
            self.x_vars[state].set(DEFAULT_TUNING[state]["x"])
            self.y_vars[state].set(DEFAULT_TUNING[state]["y"])
        self.refresh_all()
        self.status_var.set("Reset on screen. Press Save to write files.")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sprite-dir", type=Path, default=Path("docs/cat_sprites"))
    parser.add_argument("--assets-dir", type=Path, default=Path("firmware/boards/shared/ui_status/assets"))
    parser.add_argument("--size", type=int, default=112)
    args = parser.parse_args()

    paths = Paths(sprite_dir=args.sprite_dir, assets_dir=args.assets_dir, size=args.size)
    missing = [raw_path(paths, index, state) for index, state in enumerate(STATE_NAMES) if not raw_path(paths, index, state).exists()]
    if missing:
        raise SystemExit(f"Missing raw sprite(s): {', '.join(str(path) for path in missing)}")

    root = tk.Tk()
    SpriteTuner(root, paths)
    root.mainloop()


if __name__ == "__main__":
    main()
