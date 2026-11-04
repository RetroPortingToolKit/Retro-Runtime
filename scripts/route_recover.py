"""Copy the complete field prefix of a crashed private route into a sealed file."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def recover(source: Path, destination: Path) -> int:
    if source.stat().st_size > 256 * 1024 * 1024:
        raise ValueError('route exceeds 256 MiB')
    data = source.read_bytes()
    if b'\nEND ' in data:
        raise ValueError('route is already sealed')
    lines = data.splitlines(keepends=True)
    if not lines or lines[0] != b'RCORE_ROUTE 1\n':
        raise ValueError('unsupported route header')
    index, end, count, boot = 1, 0, 0, False
    while index < len(lines):
        row = lines[index].split()
        if not row or not lines[index].endswith(b'\n'):
            break
        if row[0] in (b'I', b'A', b'S') and not boot:
            index += 1
            continue
        if row[0] not in (b'B', b'F') or len(row) != 10:
            raise ValueError(f'invalid field header at line {index + 1}')
        field, pads, clocks = int(row[1]), int(row[4]), int(row[5])
        if not 0 <= pads <= 4096 or not 0 <= clocks <= 4096:
            raise ValueError('invalid callback count')
        if row[0] == b'B':
            if boot or field:
                raise ValueError('invalid boot field')
        elif not boot or field != count + 1:
            raise ValueError('field order mismatch')
        boundary = index + 1 + pads + clocks
        if boundary > len(lines) or not all(line.endswith(b'\n') for line in lines[index:boundary]):
            break
        if any(not line.startswith(b'P ') for line in lines[index + 1:index + 1 + pads]):
            raise ValueError('invalid pad record')
        if any(not line.startswith(b'T ') for line in lines[index + 1 + pads:boundary]):
            raise ValueError('invalid clock record')
        boot = True
        if row[0] == b'F':
            count += 1
            end = boundary
        index = boundary
    if not count:
        raise ValueError('no complete field to recover')
    prefix = b''.join(lines[:end])
    footer = f'END {count} {hashlib.sha256(prefix).hexdigest()}\n'.encode('ascii')
    # Exclusive creation preserves both the original and any existing output.
    with destination.open('xb') as output:
        output.write(prefix)
        output.write(footer)
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('partial', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        frames = recover(args.partial, args.output)
    except (OSError, ValueError) as error:
        parser.exit(2, f'recovery refused: {error}\n')
    print(f'recovered {frames} complete fields; the original is unchanged')


if __name__ == '__main__':
    main()
