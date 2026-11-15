"""Reapply the ULUS10020 remote player playback hooks after AOT regeneration.

Both sites are in the remote player controller (unit 0050):
  0x088CCF94  interpolation of the remote position       -> update_replica_playback_clock
  0x088CE05C  "the cursor is past the newest sample"     -> replica_keep_history

The second one decides whether the controller clears its sample history when
no new sample arrived. The 50 ms cadence constants of the same unit (see
diagnostics/multiplayer/VALIDATION.md) are separate edits and are not made here.
"""
from pathlib import Path

path = Path(__file__).resolve().parent / 'PSPRecomp/profiles/kokaku/generated/generated_unit_0050.cpp'
include = '#include "kokaku_replica_timing.hpp"\n'
anchor_include = '#include "generated_units.hpp"\n'

SPENT_TEST = ('    ctx.gpr[2] = (aot_mem.aot_load32(ctx.gpr[4] + static_cast<std::uint32_t>(36)));\n'
              '    ctx.gpr[2] = (ctx.gpr[2] < ctx.gpr[5] ? 1u : 0u);\n')
SPENT_NEXT = ('    { const bool branch_taken = ctx.gpr[2] == 0u;\n'
              '    ctx.gpr[2] = (0u + static_cast<std::uint32_t>(10));\n')

# (anchor before the hook, hook, anchor after the hook)
hooks = [
    ('L_088CCF94:\n', '    kokaku::update_replica_playback_clock(rt.memory(), ctx.gpr[4]);\n', ''),
    (SPENT_TEST, '    ctx.gpr[2] = kokaku::replica_keep_history(rt.memory(), ctx.gpr[18], ctx.gpr[2]);\n', SPENT_NEXT),
]

original = path.read_bytes()
crlf = b'\r\n' in original
source = original.decode().replace('\r\n', '\n')
for before, hook, after in hooks:
    if before + hook + after in source:
        continue
    if source.count(before + after) != 1:
        raise SystemExit(f'Expected unique ULUS10020 anchor missing in {path.name}: {before.strip()[:60]}; no writes made.')
    source = source.replace(before + after, before + hook + after, 1)
if include not in source:
    if source.count(anchor_include) != 1:
        raise SystemExit(f'Generated include anchor missing in {path.name}; no writes made.')
    source = source.replace(anchor_include, anchor_include + include, 1)
updated = (source.replace('\n', '\r\n') if crlf else source).encode()
if updated != original:
    path.write_bytes(updated)
print('Kokaku remote player hooks present: playback clock, keep history')
