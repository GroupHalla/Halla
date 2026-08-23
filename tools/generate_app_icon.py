#!/usr/bin/env python3
"""Generate the large Windows/runtime Halla icon from the official logo."""
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[1]
source_path = root / "src/assets/halla-logo.png"
png_path = root / "src/assets/halla-app-icon.png"
ico_path = root / "src/halla.ico"

source = Image.open(source_path).convert("RGBA")
alpha = source.getchannel("A")
bounds = alpha.point(lambda value: 255 if value > 8 else 0).getbbox()
if not bounds:
    raise SystemExit("official logo has no visible pixels")

cropped = source.crop(bounds)
# Fill the complete icon width. The logo's aspect ratio is preserved, leaving
# only the unavoidable small vertical margin on a square Windows icon canvas.
width = 1024
height = max(1, round(cropped.height * width / cropped.width))
scaled = cropped.resize((width, height), Image.Resampling.LANCZOS)
canvas = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
canvas.alpha_composite(scaled, (0, (1024 - height) // 2))
canvas.save(png_path, optimize=True)
canvas.save(
    ico_path,
    format="ICO",
    sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40),
           (48, 48), (64, 64), (128, 128), (256, 256)],
)
print(f"generated {png_path.relative_to(root)} and {ico_path.relative_to(root)}")
