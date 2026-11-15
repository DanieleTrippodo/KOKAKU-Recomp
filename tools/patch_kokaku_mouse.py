"""Reapply local-player mouse and weapon hooks after regenerating Kokaku AOT units."""
from pathlib import Path

path = Path(__file__).resolve().parent / 'PSPRecomp/profiles/kokaku/generated/generated_unit_0030.cpp'
source = path.read_text()
include = '#include "kokaku_mouse.hpp"'
hook = '    kokaku::update_mouse_look(rt.memory(), ctx.gpr[4], ctx.gpr[5]);'
weapon_hook = '    if (kokaku::update_weapon_cycle(rt, ctx, aot_mem.aot_load32(ctx.gpr[19] + 192u))) return;'
if include not in source:
    source = include + '\n' + source
if hook not in source:
    anchor = '''    ctx.gpr[4] = (aot_mem.aot_load32(ctx.gpr[19] + static_cast<std::uint32_t>(192)));
    ctx.gpr[5] = (ctx.gpr[17] + 0u);'''
    if source.count(anchor) != 1:
        raise SystemExit('Expected ULUS10020 motion commit site not found; source was not changed.')
    source = source.replace(anchor, anchor + '\n' + hook)
if weapon_hook not in source:
    anchor = 'L_0887C570:'
    if source.count(anchor) != 1:
        raise SystemExit('Expected ULUS10020 weapon hook site not found; source was not changed.')
    source = source.replace(anchor, anchor + '\n' + weapon_hook)
path.write_text(source)
print('Kokaku mouse and weapon hooks present at 0x0887C570')
