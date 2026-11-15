import struct
import os
import sys

def extract_data_img(data_img_path, prx_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)

    with open(data_img_path, 'rb') as f:
        # Read header
        num_sections, strtab_off = struct.unpack('<HH', f.read(4))
        print(f"[+] Found {num_sections} sections in data.img (string table at offset {strtab_off})")

        sections = []
        for i in range(num_sections):
            sec_cnt, uncomp_size, sec_start = struct.unpack('<3I', f.read(12))
            sections.append({
                'id': i,
                'sec_cnt': sec_cnt,
                'uncomp_size': uncomp_size,
                'sec_start': sec_start
            })

        # Read section names
        f.seek(strtab_off)
        for i in range(num_sections):
            name_bytes = b''
            while True:
                b = f.read(1)
                if not b or b == b'\x00':
                    break
                name_bytes += b
            sections[i]['name'] = name_bytes.decode('latin-1', errors='replace')

        print("\n=== All 72 Sections in data.img ===")
        for s in sections:
            print(f"[{s['id']:2d}] {s['name']:<15} | Sector: {s['sec_start']:<6} | Sectors: {s['sec_cnt']:<4} | Size: {s['uncomp_size']:<8} bytes")

        # Save manifest of sections
        manifest_path = os.path.join(out_dir, 'sections_manifest.txt')
        with open(manifest_path, 'w', encoding='utf-8') as mf:
            mf.write(f"Sections count: {num_sections}\n")
            for s in sections:
                mf.write(f"{s['id']}\t{s['name']}\t{s['sec_start']}\t{s['sec_cnt']}\t{s['uncomp_size']}\n")
        print(f"\n[+] Manifest written to {manifest_path}")

        # Extract section 0 (title package) as demonstration and verification
        # The title package index is at sector 2 (uncompressed quick boot cache)
        print("\n=== Extracting Section 0 (title package) ===")
        f.seek(2 * 2048)
        title_pkg = f.read(2912)
        
        # Parse files inside title package
        # Strings are after offset 0x45a
        strtab = title_pkg[0x45a:]
        def get_name(off):
            end = strtab.find(b'\x00', off)
            return strtab[off:end].decode('latin-1', errors='replace') if end != -1 else ''

        sec0_dir = os.path.join(out_dir, 'title')
        os.makedirs(sec0_dir, exist_ok=True)

        extracted_count = 0
        for off in range(0x10, 0x450, 18):
            chunk = title_pkg[off:off+18]
            if len(chunk) < 18:
                break
            sec_off, sec_cnt, name_off, u_size, c_size = struct.unpack('<HI3I', chunk)
            name = get_name(name_off)
            if not name:
                continue

            # Read file from data.img
            file_sector = 2 + sec_off
            f.seek(file_sector * 2048)
            file_bytes = f.read(c_size if c_size > 0 else u_size)

            file_path = os.path.join(sec0_dir, name)
            with open(file_path, 'wb') as out_f:
                out_f.write(file_bytes)
            extracted_count += 1
            status = f"compressed ({c_size} -> {u_size})" if c_size > 0 else "uncompressed"
            print(f"  -> Extracted: {name:<25} ({len(file_bytes)} bytes, {status})")

        print(f"[+] Successfully extracted {extracted_count} files for title section into {sec0_dir}")

if __name__ == '__main__':
    data_img = 'extracted/PSP_GAME/USRDIR/data.img'
    prx = 'extracted/PSP_GAME/USRDIR/main.psp.prx'
    out = 'extracted_assets'
    extract_data_img(data_img, prx, out)
