"""Regenerate the four main AOT units that contain previously unsupported VCRS."""
from pathlib import Path
import subprocess
import shutil

root = Path(__file__).resolve().parent.parent
backup = root / 'diagnostics/before-first-mission/generated'
backup.mkdir(parents=True, exist_ok=True)
raw = root / 'diagnostics/main-mission-generated'
subprocess.run([str(root / 'tools/PSPRecomp/build/psp_recomp.exe'),
                str(root / 'extracted/PSP_GAME/USRDIR/main.psp.prx'),
                '--auto', str(raw), '0x08804000', '16384'], check=True)
# Regenerate complete blocks: unsupported opcodes previously terminated CFG
# discovery, so replacing only the stop would skip the following instructions.
for unit in (129, 162, 163, 165):
    path = root / f'tools/PSPRecomp/profiles/kokaku/generated/generated_unit_{unit:04d}.cpp'
    saved = backup / path.name
    if not saved.exists():
        shutil.copy2(path, saved)
    after = (raw / path.name).read_text()
    if after != path.read_text():
        path.write_text(after, newline='\n')
        print(path.name)
