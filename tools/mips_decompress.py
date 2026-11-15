import struct

class MipsDecompressor:
    def __init__(self, prx_path):
        with open(prx_path, 'rb') as f:
            # Code is at virtual address 0x000309ec, file offset 0xc0 + (0x000309ec - 0x40)
            f.seek(0xc0 + (0x000309ec - 0x40))
            self.code_bytes = f.read(174 * 4)
        self.code = [struct.unpack('<I', self.code_bytes[i*4:(i+1)*4])[0] for i in range(174)]
        self.base_pc = 0x000309ec

    def decompress(self, src_bytes, max_dst_size=4*1024*1024):
        src_buf = bytearray(src_bytes)
        dst_buf = bytearray(max_dst_size)

        r = [0] * 32
        SRC_BASE = 0x10000000
        DST_BASE = 0x20000000
        r[4] = DST_BASE # a0 (dst)
        r[5] = SRC_BASE # a1 (src)
        r[31] = 0xdeadbeef # ra (return address)

        pc = self.base_pc
        cycles = 0

        def s32(val):
            val &= 0xffffffff
            return val if val < 0x80000000 else val - 0x100000000

        def u32(val):
            return val & 0xffffffff

        def mem_read_u8(addr):
            if SRC_BASE <= addr < SRC_BASE + len(src_buf):
                return src_buf[addr - SRC_BASE]
            elif DST_BASE <= addr < DST_BASE + len(dst_buf):
                return dst_buf[addr - DST_BASE]
            else:
                raise RuntimeError(f"Invalid read at 0x{addr:08x}")

        def mem_read_s8(addr):
            v = mem_read_u8(addr)
            return v if v < 128 else v - 256

        def mem_write_u8(addr, val):
            val &= 0xff
            if DST_BASE <= addr < DST_BASE + len(dst_buf):
                dst_buf[addr - DST_BASE] = val
            else:
                raise RuntimeError(f"Invalid write at 0x{addr:08x}")

        def exec_inst(inst, cur_pc):
            nonlocal pc
            op = (inst >> 26) & 0x3f
            rs = (inst >> 21) & 0x1f
            rt = (inst >> 16) & 0x1f
            rd = (inst >> 11) & 0x1f
            sh = (inst >> 6) & 0x1f
            fn = inst & 0x3f
            imm = inst & 0xffff
            simm = imm if imm < 0x8000 else imm - 0x10000
            target = (inst & 0x3ffffff) << 2

            branch_taken = False
            branch_target = None

            if op == 0:
                if fn == 0x00: r[rd] = u32(r[rt] << sh)
                elif fn == 0x02: r[rd] = u32(r[rt] >> sh)
                elif fn == 0x03: r[rd] = u32(s32(r[rt]) >> sh)
                elif fn == 0x08:
                    branch_taken = True
                    branch_target = r[rs]
                elif fn == 0x21: r[rd] = u32(r[rs] + r[rt])
                elif fn == 0x24: r[rd] = u32(r[rs] & r[rt])
                elif fn == 0x25: r[rd] = u32(r[rs] | r[rt])
                else: raise NotImplementedError(f"Special fn=0x{fn:02x}")
            elif op == 0x01:
                if rt == 0 and s32(r[rs]) < 0:
                    branch_taken = True
                    branch_target = cur_pc + 4 + (simm << 2)
                elif rt == 1 and s32(r[rs]) >= 0:
                    branch_taken = True
                    branch_target = cur_pc + 4 + (simm << 2)
            elif op == 0x02:
                branch_taken = True
                branch_target = target
            elif op == 0x04:
                if r[rs] == r[rt]:
                    branch_taken = True
                    branch_target = cur_pc + 4 + (simm << 2)
            elif op == 0x05:
                if r[rs] != r[rt]:
                    branch_taken = True
                    branch_target = cur_pc + 4 + (simm << 2)
            elif op == 0x09: r[rt] = u32(r[rs] + simm)
            elif op == 0x0a: r[rt] = 1 if s32(r[rs]) < simm else 0
            elif op == 0x0b: r[rt] = 1 if u32(r[rs]) < u32(simm) else 0
            elif op == 0x0c: r[rt] = u32(r[rs] & imm)
            elif op == 0x0f: r[rt] = u32(imm << 16)
            elif op == 0x20: r[rt] = u32(mem_read_s8(r[rs] + simm))
            elif op == 0x24: r[rt] = u32(mem_read_u8(r[rs] + simm))
            elif op == 0x28: mem_write_u8(r[rs] + simm, r[rt] & 0xff)
            else: raise NotImplementedError(f"Op=0x{op:02x}")

            r[0] = 0
            return branch_taken, branch_target

        while pc != 0xdeadbeef and cycles < 200000000:
            cycles += 1
            idx = (pc - self.base_pc) // 4
            inst = self.code[idx]
            cur_pc = pc

            branch_taken, branch_target = exec_inst(inst, cur_pc)
            if branch_taken:
                # execute delay slot instruction
                d_inst = self.code[idx + 1]
                exec_inst(d_inst, cur_pc + 4)
                pc = branch_target
            else:
                pc = cur_pc + 4

        out_size = r[2]
        return bytes(dst_buf[:out_size])
