"""Extract FAP metadata and required APIs without executing or decompiling it.

Requires pyelftools. Accepts individual FAPs or directories of FAPs. A symbol
match is only an API inventory check, not proof the interpreter can run an app.
"""
import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from elftools.common.exceptions import ELFError
from audit_arm_fap_api import bridges


def inspect(path, supported):
    data = path.read_bytes()
    result = {'path': str(path), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    try:
        elf = ELFFile(io.BytesIO(data))
        if elf['e_machine'] != 'EM_ARM' or elf.elfclass != 32 or not elf.little_endian:
            raise ValueError('Not a little-endian ARM ELF32 file')
        meta = elf.get_section_by_name('.fapmeta')
        if meta is None:
            raise ValueError('Missing FAP manifest')
        manifest = meta.data()
        if len(manifest) < 85 or struct.unpack_from('<II', manifest) != (0x52474448, 1):
            raise ValueError('Invalid FAP manifest')
        minor, major, target, stack = struct.unpack_from('<HHHH', manifest, 8)
        symbols = elf.get_section_by_name('.symtab')
        if symbols is None:
            raise ValueError('Missing symbol table')
        imports = sorted({s.name for s in symbols.iter_symbols()
                          if s.name and s['st_shndx'] == 'SHN_UNDEF'})
        allocated = sum(s['sh_size'] for s in elf.iter_sections() if s['sh_flags'] & 2)
        result.update(name=manifest[20:52].split(b'\0', 1)[0].decode('utf-8', 'replace'),
                      api=f'{major}.{minor}', target=target, declared_stack_bytes=stack,
                      allocated_section_bytes=allocated, imports=imports,
                      missing_imports=sorted(set(imports)-supported),
                      api_profile_accepted=(major, minor) in {(87, 0), (87, 1), (88, 0), (88, 1), (88, 2)})
    except (ELFError, ValueError, struct.error, IndexError) as exc:
        result['error'] = str(exc)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', type=Path, nargs='+')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    paths = sorted({file for path in args.paths
                    for file in (path.rglob('*.fap') if path.is_dir() else [path])})
    supported = bridges()
    reports = [inspect(path, supported) for path in paths]
    frequency = Counter(name for report in reports for name in report.get('missing_imports', []))
    result = {'bridged_symbol_count': len(supported), 'file_count': len(reports),
              'note': 'Static inventory only. CPU instructions, ABI behavior, memory limits and controls also require testing.',
              'missing_api_frequency': dict(frequency.most_common()), 'files': reports}
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
        print(f'{len(reports)} FAPs inspected; {sum("error" in r for r in reports)} invalid/non-ARM files; report: {args.output}')
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
