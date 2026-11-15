"""Read the nested data.img indexes without extracting or modifying game files."""
import argparse
import collections
import json
import struct
from pathlib import Path


def inspect(path, include_files=False):
    file_size = path.stat().st_size
    with path.open('rb') as source:
        def read(offset, size):
            if offset < 0 or size < 0 or offset + size > file_size:
                raise ValueError(f'Index exceeds data.img at {offset:#x}')
            source.seek(offset)
            return source.read(size)

        count, strings = struct.unpack('<HH', read(0, 4))
        header = read(0, strings)
        names = read(strings, 4096).split(b'\0')[:count]
        files = []
        for package, name in enumerate(names):
            indexes, total_size, pointer_word = struct.unpack_from('<III', header, 4 + package * 12)
            offsets = struct.unpack_from('<' + 'I' * indexes, header, pointer_word * 4)
            for subindex, offset in enumerate(offsets):
                directories, entries, string_size = struct.unpack('<HHI', read(offset, 8))
                table = 8 + directories * 12
                string_start = table + entries * 18
                block = read(offset, string_start + string_size)
                for entry in range(entries):
                    name_offset, size, compressed, data_offset, directory = struct.unpack_from('<IIIIH', block, table + entry * 18)
                    if name_offset >= string_size or data_offset + (compressed or size) > file_size:
                        raise ValueError('Invalid file index entry')
                    filename = block[string_start + name_offset:].split(b'\0', 1)[0].decode('ascii')
                    files.append(dict(package=name.decode('ascii'), index=subindex, name=filename,
                                      offset=data_offset, size=size, compressed_size=compressed))
        # Packages share subindexes: entry references are not unique assets.
        unique = {(f['offset'], f['size'], f['compressed_size'], f['name']): f for f in files}
        extensions = collections.Counter(Path(f['name']).suffix.lower() for f in unique.values())
        result = dict(source=str(path), packages=count, entry_references=len(files), unique_assets=len(unique),
                      extensions=dict(sorted(extensions.items())),
                      pmf=[f for f in unique.values() if f['name'].lower().endswith('.pmf')],
                      cinematic_packages=sorted({f['package'] for f in files
                                                 if f['package'].startswith(('mv', 'movie'))}))
        if include_files:
            result['files'] = files
        return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('report', type=Path)
    parser.add_argument('--full', action='store_true', help='Include every package entry reference')
    args = parser.parse_args()
    result = inspect(args.image, args.full)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}, indent=2))
