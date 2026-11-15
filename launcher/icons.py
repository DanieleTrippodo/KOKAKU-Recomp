"""Encode the approved artwork as multi-resolution ICO and embed PE resources.

Only format conversion/downsampling is performed here; the game artwork and
its alpha are preserved. The setup artwork was adapted with ImageGen.
"""
import ctypes
from ctypes import wintypes as w
from pathlib import Path
import struct
from PIL import Image

ASSETS = Path(__file__).resolve().parent / "assets"
SIZES = [(n, n) for n in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)]


def encode_icons():
    for name in ("setup", "game"):
        with Image.open(ASSETS / f"{name}-icon.png") as source:
            # ICO export encodes each Windows-requested size from this master.
            # Pad the 511x512 game source by one transparent column; no stretching.
            side = max(source.size)
            master = Image.new("RGBA", (side, side), (0, 0, 0, 0))
            master.alpha_composite(source.convert("RGBA"), ((side-source.width)//2, (side-source.height)//2))
            master.save(ASSETS / f"{name}.ico", format="ICO", sizes=SIZES)


def embed_icon(exe, icon, group_id=1):
    """Update only RT_ICON / RT_GROUP_ICON, preserving all other PE resources."""
    raw = Path(icon).read_bytes()
    reserved, kind, count = struct.unpack_from("<HHH", raw)
    if reserved != 0 or kind != 1 or count < 1:
        raise ValueError("Invalid ICO header")
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.BeginUpdateResourceW.argtypes = [w.LPCWSTR, w.BOOL]
    k.BeginUpdateResourceW.restype = w.HANDLE
    k.UpdateResourceW.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                 w.WORD, ctypes.c_void_p, w.DWORD]
    k.UpdateResourceW.restype = w.BOOL
    k.EndUpdateResourceW.argtypes = [w.HANDLE, w.BOOL]
    k.EndUpdateResourceW.restype = w.BOOL
    handle = k.BeginUpdateResourceW(str(Path(exe).resolve()), False)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        group = bytearray(struct.pack("<HHH", 0, 1, count))
        for index in range(count):
            entry = raw[6 + 16 * index:22 + 16 * index]
            size, offset = struct.unpack_from("<II", entry, 8)
            if len(entry) != 16 or offset + size > len(raw):
                raise ValueError("Invalid ICO entry")
            image_id = 100 + index
            image = ctypes.create_string_buffer(raw[offset:offset + size])
            if not k.UpdateResourceW(handle, 3, image_id, 0x409, image, size):
                raise ctypes.WinError(ctypes.get_last_error())
            group += entry[:12] + struct.pack("<H", image_id)
        data = ctypes.create_string_buffer(bytes(group))
        if not k.UpdateResourceW(handle, 14, group_id, 0x409, data, len(group)):
            raise ctypes.WinError(ctypes.get_last_error())
    except BaseException:
        k.EndUpdateResourceW(handle, True)
        raise
    if not k.EndUpdateResourceW(handle, False):
        raise ctypes.WinError(ctypes.get_last_error())


def verify_icon(exe, icon, group_id=1):
    """Read back the embedded images as data, never execute the target EXE."""
    raw = Path(icon).read_bytes()
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.LoadLibraryExW.argtypes = [w.LPCWSTR, w.HANDLE, w.DWORD]
    k.LoadLibraryExW.restype = w.HMODULE
    k.FindResourceW.argtypes = [w.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    k.FindResourceW.restype = w.HRSRC
    k.LoadResource.argtypes = [w.HMODULE, w.HRSRC]
    k.LoadResource.restype = w.HGLOBAL
    k.LockResource.argtypes = [w.HGLOBAL]
    k.LockResource.restype = ctypes.c_void_p
    k.SizeofResource.argtypes = [w.HMODULE, w.HRSRC]
    k.SizeofResource.restype = w.DWORD
    k.FreeLibrary.argtypes = [w.HMODULE]
    module = k.LoadLibraryExW(str(Path(exe).resolve()), None, 0x22)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    def read(kind, name):
        resource = k.FindResourceW(module, name, kind)
        if not resource:
            raise AssertionError(f"Missing resource {kind}/{name}")
        return ctypes.string_at(k.LockResource(k.LoadResource(module, resource)),
                                k.SizeofResource(module, resource))
    try:
        group = read(14, group_id)
        assert group[:6] == raw[:6]
        count = struct.unpack_from("<H", raw, 4)[0]
        for index in range(count):
            entry = raw[6 + 16 * index:22 + 16 * index]
            actual = group[6 + 14 * index:20 + 14 * index]
            size, offset = struct.unpack_from("<II", entry, 8)
            # windres normalizes the PNG ICO's unspecified planes (0) to 1.
            assert actual[:4] == entry[:4] and actual[6:12] == entry[6:12]
            assert struct.unpack_from("<H", actual, 4)[0] in (0, 1)
            image_id = struct.unpack_from("<H", actual, 12)[0]
            assert read(3, image_id) == raw[offset:offset + size]
        return count
    finally:
        k.FreeLibrary(module)


if __name__ == "__main__":
    encode_icons()
