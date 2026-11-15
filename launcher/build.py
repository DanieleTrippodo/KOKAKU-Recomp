"""Build the standalone native preparer using the project's bundled MinGW tools."""
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from icons import encode_icons, embed_icon, verify_icon

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = HERE / "build"
DIST = ROOT / "dist"
RECOMP = ROOT / "tools/PSPRecomp"
BIN = RECOMP / "build/bin"
TOOL = ROOT / "tools/w64devkit/bin"
BUILD.mkdir(exist_ok=True)
DIST.mkdir(exist_ok=True)
encode_icons()

def run(*args):
    subprocess.run([str(a) for a in args], check=True, cwd=HERE,
                   env=dict(os.environ, PATH=str(TOOL) + os.pathsep + os.environ["PATH"]))

# Only this explicit allowlist enters the package. No ISO, extracted assets,
# mods, modloader, user settings, saved games or diagnostic files are bundled.
game = BUILD / "KOKAKU Recomp.exe"
shutil.copyfile(BIN / "KokakuNative.exe", game)
run(TOOL / "strip.exe", "--strip-all", game)
# Use the GUI subsystem so a direct double-click does not open a console.
data = bytearray(game.read_bytes())
pe = struct.unpack_from("<I", data, 0x3c)[0]
assert data[pe:pe+4] == b"PE\0\0"
struct.pack_into("<H", data, pe + 24 + 68, 2)
game.write_bytes(data)
embed_icon(game, HERE / "assets/game.ico")
verify_icon(game, HERE / "assets/game.ico")
files = {"KOKAKU Recomp.exe": game.read_bytes()}
for name in ["avcodec-61.dll", "avformat-61.dll", "avutil-59.dll", "swresample-5.dll",
             "swscale-8.dll", "dxcompiler.dll", "dxil.dll"]:
    files[name] = (BIN / name).read_bytes()
files["KokakuNative.ini"] = (RECOMP / "profiles/kokaku/config/KokakuNative.ini").read_bytes()
files["README.txt"] = (HERE / "PORTABLE_README.txt").read_bytes()
files["licenses/PSPRecomp-MIT.txt"] = (RECOMP / "LICENSE").read_bytes()
files["licenses/FFmpeg-LGPL-2.1.txt"] = (RECOMP / "profiles/kokaku/third_party/ffmpeg/COPYING.LGPLv2.1").read_bytes()
files["licenses/DepartureMono-OFL.txt"] = (HERE / "assets/DepartureMono-1.500/LICENSE").read_bytes()
for name in ["LICENSE-MS.txt", "LICENSE-LLVM.txt", "LICENCE-MIT.txt", "README.md"]:
    files["licenses/dxc/" + name] = (RECOMP / "profiles/kokaku/third_party/dxc" / name).read_bytes()
files["licenses/THIRD-PARTY.txt"] = (HERE / "THIRD-PARTY.txt").read_bytes()

cab = ctypes.WinDLL("cabinet", use_last_error=True)
cab.CreateCompressor.argtypes = [w.DWORD, ctypes.c_void_p, ctypes.POINTER(w.HANDLE)]
cab.CreateCompressor.restype = w.BOOL
cab.Compress.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
cab.Compress.restype = w.BOOL
cab.CloseCompressor.argtypes = [w.HANDLE]

def compress(raw):
    handle = w.HANDLE()
    if not cab.CreateCompressor(4, None, ctypes.byref(handle)): # XPRESS_HUFF, Windows 8+
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        size = ctypes.c_size_t()
        source = ctypes.create_string_buffer(raw)
        cab.Compress(handle, source, len(raw), None, 0, ctypes.byref(size))
        out = ctypes.create_string_buffer(size.value)
        if not cab.Compress(handle, source, len(raw), out, len(out), ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        return out.raw[:size.value]
    finally:
        cab.CloseCompressor(handle)

package = bytearray(b"KOKAKU01" + struct.pack("<I", len(files)))
manifest = []
for name, raw in files.items():
    packed = compress(raw)
    path = name.encode("utf-8")
    digest = hashlib.sha256(raw).digest()
    package += struct.pack("<IQQ", len(path), len(raw), len(packed)) + digest + path + packed
    manifest.append(dict(path=name, size=len(raw), sha256=digest.hex()))
    print(f"Pack {name}: {len(raw):,} -> {len(packed):,}", flush=True)
(BUILD / "payload.bin").write_bytes(package)
(BUILD / "package-manifest.json").write_text(json.dumps(manifest, indent=2))

usa = json.loads((HERE / "usa-manifest.json").read_text())
(BUILD / "usa_manifest.hpp").write_text(
    "// Exact source hashes for the supported ULUS10020 revision. No game data.\n"
    "static const Expected kExpected[] = {\n" + "".join(
        '    {"%s", %du, "%s"},\n' % (x["path"], x["size"], x["sha256"]) for x in usa) + "};\n")
run(TOOL / "windres.exe", "resources.rc", "-O", "coff", "-o", BUILD / "resources.o")
run(TOOL / "g++.exe", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wpedantic",
    "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0A00", "-municode", "-mwindows",
    "-static", "-static-libgcc", "-static-libstdc++", "-s", "main.cpp", "core.cpp",
    BUILD / "resources.o", "-o", DIST / "KOKAKU Recomp Setup.exe",
    "-lbcrypt", "-lcabinet", "-lcomdlg32", "-lole32", "-luuid", "-lshell32", "-lgdi32", "-lcomctl32", "-ldwmapi")
verify_icon(DIST / "KOKAKU Recomp Setup.exe", HERE / "assets/setup.ico")
shutil.copyfile(HERE / "README.md", DIST / "README.md")
print(f"Built {DIST / 'KOKAKU Recomp Setup.exe'} ({(DIST / 'KOKAKU Recomp Setup.exe').stat().st_size:,} bytes)")
