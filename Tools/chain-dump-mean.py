#!/usr/bin/env python3
"""Mean channel value of the librashader chain-image dumps, for photometric comparisons.

Measures the PNG pairs that `LibrashaderDumpChainImages` writes under <User>/Dump/Textures/, and the
OS-level window captures taken beside them, so that "the shader renders too dark" becomes a number.
The result of using it is recorded in
docs/superpowers/specs/2026-09-16-librashader-desktop-runtimes-design.md section 7.

Why ffmpeg rather than PIL: no third-party Python dependency, ffmpeg decodes PNG exactly, and
averaging raw bytes is verifiable against a synthetic flat field. Generate such a field as rawvideo,
not with ffmpeg's lavfi `color=` source -- that one goes through YUV and reports 126 for 128.

Usage:
    ./chain-dump-mean.py [--crop X,Y,W,H] [--rgb10a2] <image.png> [<image.png> ...]
    ./chain-dump-mean.py [--crop X,Y,W,H] [--rgb10a2-output] --ratio <input.png> <output.png>

--ratio prints the output/input mean ratio as a percentage, which is the number that section's
tables are expressed in. It does NOT require the two images to be the same size: the chain routinely
scales, and a mean is resolution-independent.

--crop restricts every image to a rectangle before averaging, and exists for the OS-level window
captures: those include the title bar and the letterbox borders, which are near-black and would pull
both means toward zero, compressing any ratio toward 100% and hiding exactly the effect being
measured. The chain dumps need no crop -- they are the chain's own images.

--rgb10a2 says the PNG's bytes are not really RGBA8 but packed A2B10G10R10 words, and unpacks them
before averaging. This is needed for the chain OUTPUT dump, and is the difference between a real
measurement and a fabricated one. The chain output target inherits the backbuffer's format, and
AbstractTexture::Save copies the source texture's native bytes into an RGBA8 staging texture and
hands them to the PNG encoder unconverted -- so on a 10-bit swapchain (this UAT host's Vulkan and
D3D12 both pick one) the PNG is a valid image file full of misread 10-bit words. It looks plausible:
measured 2026-09-18, a pure passthrough preset's output dump read as RGBA8 came out 160% of its own
input, and unpacking it reproduced the input to within 0.05/255. The giveaway is the alpha channel --
an opaque 10-bit image read as RGBA8 has alpha in exactly 192..255, because a 2-bit alpha of 3 sits
in the top two bits of the fourth byte. Values are rescaled to 0-255 so they compare directly with
the 8-bit path. Never pass this for a screen capture or a chain INPUT dump; those really are RGBA8,
and unpacking one gives nonsense (that is a useful control: the input dump unpacks to B 251.8).
--rgb10a2-output is the --ratio form: it unpacks only <output.png>, which is the pairing that
actually occurs, since the input dump is the RGBA8 EFB texture and only the output target follows the
backbuffer.

Reports per-channel means as well as the overall mean, because a chain that darkens uniformly and a
chain that has lost a channel are different bugs and the overall mean alone cannot tell them apart.
"""

import struct
import subprocess
import sys


def decode(path, crop, pixel_format):
    """Return `path` decoded to raw `pixel_format` bytes, cropped, via ffmpeg."""
    command = ["ffmpeg", "-v", "error", "-i", path]
    if crop is not None:
        x, y, w, h = crop
        command += ["-vf", f"crop={w}:{h}:{x}:{y}"]
    command += ["-f", "rawvideo", "-pix_fmt", pixel_format, "-"]
    proc = subprocess.run(command, capture_output=True)
    if proc.returncode != 0:
        raise SystemExit(f"ffmpeg failed on {path}:\n{proc.stderr.decode(errors='replace')}")
    if not proc.stdout:
        raise SystemExit(f"ffmpeg produced no pixels for {path}")
    return proc.stdout


def mean_channels(path, crop=None, rgb10a2=False):
    """Return (mean_r, mean_g, mean_b, mean_overall, pixel_count) for a PNG, via ffmpeg."""
    if rgb10a2:
        raw = decode(path, crop, "rgba")
        if len(raw) % 4 != 0:
            raise SystemExit(f"{path}: {len(raw)} bytes is not a whole number of 32-bit words")
        pixels = len(raw) // 4
        sums = [0, 0, 0]
        for word in struct.unpack(f"<{pixels}I", raw):
            sums[0] += word & 0x3FF
            sums[1] += (word >> 10) & 0x3FF
            sums[2] += (word >> 20) & 0x3FF
        # Rescale 10-bit to 0-255 rather than reporting a second, incomparable scale.
        means = [s / pixels / (1023.0 / 255.0) for s in sums]
    else:
        raw = decode(path, crop, "rgb24")
        if len(raw) % 3 != 0:
            raise SystemExit(f"{path}: {len(raw)} bytes is not a whole number of rgb24 pixels")
        pixels = len(raw) // 3
        means = [sum(raw[channel::3]) / pixels for channel in range(3)]
    return means[0], means[1], means[2], sum(means) / 3.0, pixels


def report(path, crop=None, rgb10a2=False):
    r, g, b, overall, pixels = mean_channels(path, crop, rgb10a2)
    notes = "" if crop is None else "  crop=%d,%d,%d,%d" % crop
    notes += "  as A2B10G10R10" if rgb10a2 else ""
    print(f"{path}{notes}")
    print(f"  pixels {pixels}")
    print(f"  mean R {r:8.3f}  G {g:8.3f}  B {b:8.3f}")
    print(f"  mean overall {overall:8.3f}")
    return overall


def parse_crop(spec):
    try:
        x, y, w, h = (int(part) for part in spec.split(","))
    except ValueError:
        raise SystemExit(f"--crop wants four integers X,Y,W,H, got {spec!r}") from None
    if w <= 0 or h <= 0:
        raise SystemExit(f"--crop needs a positive width and height, got {w}x{h}")
    return x, y, w, h


def main(argv):
    args = argv[1:]
    crop = None
    # Two flags rather than one, because the pairing is asymmetric: a chain input dump really is
    # RGBA8 while the chain output dump beside it is packed, so a single flag spanning both images
    # of a --ratio could only ever be wrong for one of them.
    packed_all = False
    packed_output = False
    while args and args[0].startswith("--") and args[0] != "--ratio":
        flag = args.pop(0)
        if flag == "--crop":
            if not args:
                raise SystemExit("--crop needs an X,Y,W,H argument")
            crop = parse_crop(args.pop(0))
        elif flag == "--rgb10a2":
            packed_all = True
        elif flag == "--rgb10a2-output":
            packed_output = True
        else:
            raise SystemExit(f"unknown option {flag!r}\n{__doc__}")

    if args and args[0] == "--ratio":
        if len(args) != 3:
            raise SystemExit("--ratio takes exactly two images: <input.png> <output.png>")
        src = report(args[1], crop, packed_all)
        dst = report(args[2], crop, packed_all or packed_output)
        if src == 0:
            raise SystemExit("input mean is 0; a ratio against it is meaningless")
        print(f"\noutput/input = {100.0 * dst / src:.1f}%")
        return 0

    if packed_output:
        raise SystemExit("--rgb10a2-output only means something with --ratio; use --rgb10a2")
    if not args:
        raise SystemExit(__doc__)
    for path in args:
        report(path, crop, packed_all)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
