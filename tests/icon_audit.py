from pathlib import Path
import struct
import zlib

root = Path(__file__).resolve().parents[1]
png_path = root / "src/assets/halla-app-icon.png"
ico_path = root / "src/halla.ico"

raw = png_path.read_bytes()
assert raw.startswith(b"\x89PNG\r\n\x1a\n")
pos = 8
idat = bytearray()
width = height = bit_depth = color_type = None
while pos < len(raw):
    length = struct.unpack(">I", raw[pos:pos + 4])[0]
    kind = raw[pos + 4:pos + 8]
    data = raw[pos + 8:pos + 8 + length]
    pos += 12 + length
    if kind == b"IHDR":
        width, height, bit_depth, color_type = struct.unpack(">IIBB", data[:10])
    elif kind == b"IDAT":
        idat.extend(data)
    elif kind == b"IEND":
        break
assert (width, height, bit_depth, color_type) == (1024, 1024, 8, 6)

encoded = zlib.decompress(bytes(idat))
stride = width * 4
previous = bytearray(stride)
rows = []
offset = 0
for _ in range(height):
    filter_type = encoded[offset]
    current = bytearray(encoded[offset + 1:offset + 1 + stride])
    offset += stride + 1
    for x in range(stride):
        left = current[x - 4] if x >= 4 else 0
        up = previous[x]
        upper_left = previous[x - 4] if x >= 4 else 0
        if filter_type == 1:
            current[x] = (current[x] + left) & 0xFF
        elif filter_type == 2:
            current[x] = (current[x] + up) & 0xFF
        elif filter_type == 3:
            current[x] = (current[x] + ((left + up) // 2)) & 0xFF
        elif filter_type == 4:
            estimate = left + up - upper_left
            distances = (abs(estimate - left), abs(estimate - up), abs(estimate - upper_left))
            predictor = (left, up, upper_left)[distances.index(min(distances))]
            current[x] = (current[x] + predictor) & 0xFF
        else:
            assert filter_type == 0
    rows.append(current)
    previous = current

min_x, min_y = width, height
max_x = max_y = -1
for y, row in enumerate(rows):
    for x in range(width):
        if row[x * 4 + 3] > 8:
            min_x, max_x = min(min_x, x), max(max_x, x)
            min_y, max_y = min(min_y, y), max(max_y, y)
assert max_x >= 0
assert (max_x - min_x + 1) / width >= 0.98
assert (max_y - min_y + 1) / height >= 0.95

ico = ico_path.read_bytes()
reserved, kind, count = struct.unpack("<HHH", ico[:6])
assert (reserved, kind) == (0, 1)
assert count >= 9
sizes = set()
for index in range(count):
    entry = ico[6 + index * 16:6 + (index + 1) * 16]
    icon_width, icon_height = entry[0], entry[1]
    sizes.add((256 if icon_width == 0 else icon_width,
               256 if icon_height == 0 else icon_height))
for required in ((16, 16), (24, 24), (32, 32), (48, 48), (256, 256)):
    assert required in sizes

print(f"Icon audit OK: visible bounds {min_x},{min_y}–{max_x},{max_y}; {len(sizes)} ICO sizes")
