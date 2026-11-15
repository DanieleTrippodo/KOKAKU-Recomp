"""Restore the ULUS10020 CFrame hook after AOT regeneration, preserving other patches."""
from pathlib import Path

path = Path(__file__).resolve().parent / 'PSPRecomp/profiles/kokaku/generated/generated_unit_0076.cpp'
source = path.read_text()
include = '#include "../host/kokaku_frame_rate.hpp"\n'
anchor = ('    aot_mem.aot_store32(ctx.gpr[18] + static_cast<std::uint32_t>(108), '
          'std::bit_cast<std::uint32_t>(ctx.fpr[4]));\n')
hook = ('    // CFrame::Update: wall time is sampled once, not once per GetDeltaTime\n'
        '    // caller or audio-thread vblank. Preserve the game\'s time-scale multiplier.\n'
        '    kokaku::adjust_frame_delta(rt.memory(), ctx.gpr[18], ctx.fpr[4], ctx.fpr[0]);\n')
start = source.index('L_08935EBC:\n')
end = source.index('L_08935EFC:\n', start)
block = source[start:end]
if block.count(anchor) != 1 or source.count(anchor) != 1:
    raise SystemExit('CFrame timing anchor changed; no writes made.')
if 'kokaku::adjust_frame_delta' in source and block.count(hook) != 1:
    raise SystemExit('CFrame hook differs; no writes made.')
if hook not in block:
    source = source.replace(anchor, hook + anchor)
if include not in source:
    source = source.replace('#include "generated_units.hpp"\n', '#include "generated_units.hpp"\n' + include, 1)
    if include not in source:
        raise SystemExit('Generated include anchor missing; no writes made.')
if path.read_text() != source:
    path.write_text(source)
print('Kokaku CFrame wall-clock hook present')

path = path.with_name('generated_unit_0098.cpp')
source = path.read_text()
for address, active in [('0898C440', 'true'), ('0898C4C8', 'true'), ('0898C6C0', 'false')]:
    anchor = f'L_{address}:\n'
    hook = f'    kokaku::set_cutscene_active(rt.memory(), {active});\n'
    if source.count(anchor) != 1:
        raise SystemExit(f'Scene lifecycle anchor {address} changed; no writes made.')
    if anchor + hook not in source:
        source = source.replace(anchor, anchor + hook, 1)
if include not in source:
    source = source.replace('#include "generated_units.hpp"\n', '#include "generated_units.hpp"\n' + include, 1)
    if include not in source:
        raise SystemExit('Generated include anchor missing; no writes made.')
if path.read_text() != source:
    path.write_text(source)
print('Kokaku scene lifecycle frame-rate hooks present')
