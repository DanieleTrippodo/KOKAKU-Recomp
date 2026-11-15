"""Reapply ULUS10020 PC balance hooks without regenerating other AOT patches."""
from pathlib import Path

root = Path(__file__).resolve().parent / 'PSPRecomp/profiles/kokaku/generated'
include = '#include "kokaku_pc_balance.hpp"'
patches = {
    7: [('L_0882332C:\n',
         '    kokaku::apply_pc_balance(rt.memory(), ctx.gpr[21], kokaku::pc_balance_enabled());\n')],
    32: [('L_08884C68:\n',
          '    ctx.gpr[17] = kokaku::pc_balance_hit_damage(rt.memory(), ctx.gpr[16], ctx.gpr[20], ctx.gpr[17]);\n'),
         ('L_088846E0:\n',
          '    ctx.gpr[17] = kokaku::pc_balance_hit_damage(rt.memory(), ctx.gpr[16], ctx.gpr[18], ctx.gpr[17], 20);\n'),
         ('L_08884400:\n',
          '    ctx.gpr[21] = kokaku::pc_balance_damage(rt.memory(), ctx.gpr[17], ctx.gpr[19], ctx.gpr[21]);\n')],
    67: [('    ctx.fpr[1] = ctx.fpr[1] + ctx.fpr[20];\n'
          '    aot_mem.aot_store32(ctx.gpr[16] + static_cast<std::uint32_t>(1476), std::bit_cast<std::uint32_t>(ctx.fpr[1]));\n',
          '    ctx.fpr[1] = kokaku::pc_balance_attack_pause(rt.memory(), ctx.gpr[16], ctx.fpr[1]);\n')],
    68: [('L_08916EE4:\n', '    kokaku::pc_balance_reposition(rt.memory(), ctx.gpr[19]);\n')],
    71: [('L_08923CF8:\n', '    kokaku::pc_balance_reposition(rt.memory(), ctx.gpr[19]);\n')],
    91: [('L_08972294:\n', '    kokaku::pc_balance_trace_shot(rt.memory(), ctx.gpr[19]);\n')],
    72: [('L_089246E0:\n', '    if (kokaku::pc_balance_keep_strafing(rt.memory(), ctx.gpr[19])) ctx.gpr[2] = 0;\n')],
}
pending = {}
for unit, hooks in patches.items():
    path = root / f'generated_unit_{unit:04}.cpp'
    source = path.read_text()
    # Retire the initial probe, which counted fire requests rather than emissions.
    source = source.replace("    kokaku::pc_balance_trace_shot(rt.memory(), ctx.gpr[17], ctx.gpr[2] != 0);\n", "")
    for anchor, hook in hooks:
        # For the rest interval, insert before the store, after native arithmetic.
        before_store = 'pc_balance_attack_pause' in hook
        marker = anchor.splitlines(keepends=True)[-1] if before_store else anchor
        if source.count(marker) != 1:
            raise SystemExit(f'Expected unique ULUS10020 anchor missing in {path.name}; no changes made.')
        if hook not in source:
            if before_store:
                if source.count(anchor) != 1:
                    raise SystemExit('Attack interval arithmetic changed; no changes made.')
                source = source.replace(marker, hook + marker)
            else:
                source = source.replace(anchor, anchor + hook)
    if include not in source:
        source = include + '\n' + source
    pending[path] = source
for path, source in pending.items():
    if path.read_text() != source:
        path.write_text(source)
print('Kokaku PC balance hooks present: HP, damage, attack pause, native lateral movement and shot probe')
