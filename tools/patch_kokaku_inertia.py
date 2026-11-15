"""Restore the ULUS10020 CInertiaBhvr frame-rate hooks after AOT regeneration."""
from pathlib import Path

path = Path(__file__).resolve().parent / 'PSPRecomp/profiles/kokaku/generated/generated_unit_0037.cpp'
original = path.read_bytes()
crlf = b'\r\n' in original
source = original.decode().replace('\r\n', '\n')  # Keep the unit's own line endings on write.
include = '#include "../host/kokaku_frame_rate.hpp"\n'
# CInertiaBhvr::Move, 0x08898fc8. Both labels are return addresses inside it:
# 0x08899028 follows CFrame::GetDeltaTime (v += a * dt) and 0x088990dc follows
# the friction getter 0x0889934c (v *= friction after the motion commit).
hooks = [
    ('L_08899028:\n',
     '    ctx.fpr[0] = kokaku::inertia_acceleration_seconds(rt.memory(), ctx.gpr[16], ctx.fpr[0]);\n',
     '    ctx.fpr[2] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[16] + static_cast<std::uint32_t>(208)));\n'),
    ('L_088990DC:\n',
     '    ctx.fpr[0] = kokaku::inertia_friction(rt.memory(), ctx.fpr[0]);\n',
     '    ctx.fpr[12] = std::bit_cast<float>(std::bit_cast<std::uint32_t>(ctx.fpr[0]));\n'),
]
for label, hook, following in hooks:
    if source.count(label) != 1:
        raise SystemExit(f'Expected ULUS10020 inertia site {label.strip()} not found; no writes made.')
    if label + hook + following in source:
        continue
    if label + following not in source:
        raise SystemExit(f'Inertia site {label.strip()} differs; no writes made.')
    source = source.replace(label + following, label + hook + following, 1)
if include not in source:
    anchor = '#include "generated_units.hpp"\n'
    if source.count(anchor) != 1:
        raise SystemExit('Generated include anchor missing; no writes made.')
    source = source.replace(anchor, anchor + include, 1)
updated = (source.replace('\n', '\r\n') if crlf else source).encode()
if updated != original:
    path.write_bytes(updated)
print('Kokaku inertia frame-rate hooks present at 0x08899028 and 0x088990DC')
