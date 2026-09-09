#!/usr/bin/env python3
"""Execute the shader's scalar functions on the host, without copying their math.

Checks numerical properties and FP16 highlight retention. Set DXC to additionally
compile all entry points as SM6 HLSL. SM5 runtime compilation and texture sampling
still need Windows/game validation.
"""
import ctypes
import math
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
shader = (root / "src/codec.hlsl.h").read_text()
functions = {"HighlightShoulder": "ffI", "GamutChromaScale": "ff",
             "LuminanceGain": "ff", "ShapeLogGain": "ffff"}


def extract(name):
    start = shader.index("float " + name + "(")
    opening = shader.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (shader[end] == "{") - (shader[end] == "}")
        end += 1
    # HLSL unadorned floating literals are floats; match that in the C++ harness.
    return re.sub(r"\b(\d+\.\d+(?:e[+-]?\d+)?|\d+e[+-]?\d+)\b",
                  r"\1f", shader[start:end])


def close(actual, expected, tol=2e-6):
    assert abs(actual - expected) <= tol, (actual, expected)


with tempfile.TemporaryDirectory(prefix="neural-codec-test-") as tmp:
    cpp = Path(tmp) / "codec.cpp"
    lib = Path(tmp) / "codec.so"
    cpp.write_text("""
#include <algorithm>
#include <cmath>
using uint = unsigned;
using std::clamp;
using std::max;
using std::exp;
float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
extern "C" {
""" + "\n".join(extract(name) for name in functions) + "\n}\n")
    subprocess.run(shlex.split(os.environ.get("CXX", "g++")) +
                   ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-shared", "-fPIC", str(cpp), "-o", str(lib)], check=True)
    native = ctypes.CDLL(str(lib))
    for name, args in functions.items():
        fn = getattr(native, name)
        fn.restype = ctypes.c_float
        fn.argtypes = [ctypes.c_uint if a == "I" else ctypes.c_float for a in args]

    shoulder = native.HighlightShoulder
    for knee in (0.05, 0.3, 0.75, 0.892, 0.99):
        for curve in (0, 1):
            previous = -1.0
            for i in range(10001):
                y = i / 1000
                value = shoulder(y, knee, curve)
                assert math.isfinite(value) and 0 <= value <= 1
                assert value >= previous - 1e-7
                if y <= knee:
                    close(value, y)
                previous = value
        eps = 1e-5
        left = (shoulder(knee, knee, 1) - shoulder(knee-eps, knee, 1)) / eps
        right = (shoulder(knee+eps, knee, 1) - shoulder(knee, knee, 1)) / eps
        close(left, 1, 0.01)
        close(right, 1, 0.01)

    def encoded_half(y, curve):
        linear = shoulder(y, 0.892, curve)
        srgb = 12.92 * linear if linear <= 0.0031308 else 1.055 * linear ** (1/2.4) - 0.055
        return struct.unpack("e", struct.pack("e", srgb))[0]

    legacy = [encoded_half(y, 0) for y in (1.5, 2, 4)]
    gradual = [encoded_half(y, 1) for y in (1.5, 2, 4)]
    assert legacy == [1, 1, 1], legacy
    assert gradual[0] < gradual[1] < gradual[2] < 1, gradual

    # An oversaturated proxy must fit the gamut without changing luminance.
    weights = (0.212639, 0.715169, 0.072192)
    for rgb in ((4, 0, 0), (0, 4, 0), (0, 0, 4), (8, 2, 0.1),
                (0.5, 0.3, 0.2), (0, 0, 0), (64, 64, 64)):
        y = sum(c*w for c, w in zip(rgb, weights))
        target = shoulder(y, 0.892, 1)
        mapped = [c * target / max(y, 1e-6) for c in rgb]
        scale = native.GamutChromaScale(target, max(mapped))
        assert 0 <= scale <= 1
        result = [target + (c-target)*scale for c in mapped]
        assert all(-1e-6 <= c <= 1+1e-6 for c in result), result
        close(sum(c*w for c, w in zip(result, weights)), target)

    gain = native.LuminanceGain
    for py in (0, 1e-8, 1e-5, 0.001, 0.1, 1, 4):
        close(gain(py, py), 1)
        for ny in (0, 1e-8, 0.1, 1, 4):
            value = gain(py, ny)
            assert math.isfinite(value) and 0.125 <= value <= 8
    close(gain(0, 1), 1)
    close(gain(0.001, 1), 8)
    close(gain(1, 0), 0.125)

    shape = native.ShapeLogGain
    for center in (-3, -1, -0.1, 0, 0.1, 1, 3):
        for base in (-3, -1, 0, 1, 3):
            close(shape(center, base, 1, 1), center)
            close(shape(center, base, 0, 0), 0)
            close(shape(center, base, 0, 1), base)
            assert abs(shape(center, base, 2, 1) - center) <= 0.250001
            for detail in (0, 1, 1.15, 2):
                for lighting in (0, 1, 1.5):
                    assert -3 <= shape(center, base, detail, lighting) <= 3
        # A constant lighting gain must not be sharpened, including at extremes.
        close(shape(center, center, 2, 1), center)
    close(shape(0.1, 0, 1.15, 1), 0.115)
    close(shape(-0.1, 0, 1.15, 1), -0.115)

    print(f"codec math: all checks passed; FP16 highlights legacy={legacy}, gradual={gradual}")
    if os.environ.get("DXC"):
        hlsl = Path(tmp) / "codec.hlsl"
        hlsl.write_text(shader.split('R"HLSL(', 1)[1].rsplit(')HLSL"', 1)[0])
        entries = re.findall(r"void (CS\w+)\(uint3 tid", shader)
        assert len(entries) == 7, entries
        for entry in entries:
            subprocess.run([os.environ["DXC"], "-T", "cs_6_0", "-HV", "2018",
                            "-E", entry, "-Fo", str(Path(tmp) / (entry + ".dxil")),
                            str(hlsl)], check=True)
        print("codec HLSL: all 7 entry points compiled with DXC (SM6)")
