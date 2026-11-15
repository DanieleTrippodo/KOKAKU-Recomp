"""Reapply the ULUS10020 online-multiplayer hooks after AOT regeneration.

All four sites are in the multiplayer menu state machine (unit 0024):
  0x08865594  search page polls CKoDmeP2P::Join      -> online_join_hold (room list)
  0x0886559C  return of that poll                    -> online_join_result
  0x08865604  the search page returns its next page  -> online_join_page
  0x08865254  return of the CKoDmeP2P::Create poll   -> online_create_result
"""
from pathlib import Path

root = Path(__file__).resolve().parent / 'PSPRecomp/profiles/kokaku/generated'
include = '#include "../host/kokaku_online.hpp"\n'
anchor_include = '#include "generated_units.hpp"\n'

JOIN_CALL = ('    if (rt.invoke_chained_direct<&recomp_unit_0118_entry, 118u, 87u, 0x089DC644u>(ctx, &aot_mem)'
             ' && ctx.pc == 0x0886559Cu) goto L_0886559C;\n')

# (unit, anchor, hook, insert_before_anchor)
hooks = [
    (24, JOIN_CALL,
     '    if (kokaku::online_join_hold(rt.memory(), ctx.gpr[16])) { ctx.gpr[2] = 0u; goto L_0886559C; }\n', True),
    (24, 'L_0886559C:\n',
     '    kokaku::online_join_result(rt.memory(), ctx.gpr[2]);\n', False),
    (24, 'L_08865254:\n',
     '    kokaku::online_create_result(rt.memory(), ctx.gpr[2]);\n', False),
    (24, 'L_08865604:\n',
     '    kokaku::online_join_page(rt.memory(), ctx.gpr[4]);\n', False),
]

pending = {}
for unit, anchor, hook, before in hooks:
    path = root / f'generated_unit_{unit:04}.cpp'
    if path not in pending:
        original = path.read_bytes()
        pending[path] = [original, b'\r\n' in original, original.decode().replace('\r\n', '\n')]
    source = pending[path][2]
    if source.count(anchor) != 1:
        raise SystemExit(f'Expected unique ULUS10020 anchor missing in {path.name}: {anchor.strip()[:60]}; no writes made.')
    if hook in source:
        continue
    replacement = hook + anchor if before else anchor + hook
    pending[path][2] = source.replace(anchor, replacement, 1)

for path, (original, crlf, source) in pending.items():
    if include not in source:
        if source.count(anchor_include) != 1:
            raise SystemExit(f'Generated include anchor missing in {path.name}; no writes made.')
        source = source.replace(anchor_include, anchor_include + include, 1)
    updated = (source.replace('\n', '\r\n') if crlf else source).encode()
    if updated != original:
        path.write_bytes(updated)
print('Kokaku online multiplayer hooks present: join hold, join result, join page, create result')
