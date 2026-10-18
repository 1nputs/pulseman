#!/usr/bin/env python3
"""gen_zip_cases.py: tests/zip_cases.h, zip archives for tests/zip_unit.c to open.

Good ones made by Python's zipfile (stored and deflated entries at several levels, an archive
comment, a directory entry, an empty file), each with its files' names and the FNV-1a hashes of
their contents; and hostile ones made here by hand, each with the start of the refusal it must
give: names that climb out of a folder or are absolute, a backslash, a file twice, an encrypted
entry, a packing other than stored or deflated, a wrong CRC (refused on reading), an entry
claiming more than the limit unpacked.

    python3 tools/gen_zip_cases.py > tests/zip_cases.h
"""
import io
import struct
import zipfile
import zlib


def fnv(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def good():
    out = []
    data = {
        'league.ini': b'name = A league\nplayoff_teams = 18\n',
        'teams.csv': b'team,city\n' + b''.join(b'T%02d,City %d\n' % (i, i) for i in range(32)),
        'art/logos/ANA.png': bytes((i * 37 + i // 7) & 0xFF for i in range(3000)),
        'art/game/Sprites.bin': bytes(range(256)) * 40,
        'empty.txt': b'',
    }
    for comp, level, comment, directory in [(zipfile.ZIP_STORED, None, b'', False), (zipfile.ZIP_DEFLATED, 1, b'', False),
                                            (zipfile.ZIP_DEFLATED, 9, b'a comment after the archive', True)]:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w', comp, compresslevel=level) as z:
            if directory:
                z.writestr(zipfile.ZipInfo('art/'), b'')
            for name, b in data.items():
                z.writestr(name, b)
            z.comment = comment
        files = dict(data)
        if directory:
            files['art/'] = b''
        out.append(('%s%s%s' % ('stored' if comp == zipfile.ZIP_STORED else 'deflated, level %d' % level,
                                ', a comment' if comment else '', ', a directory' if directory else ''), buf.getvalue(), files))
    return out


def entry(name, data, method=0, flags=0, crc=None, size=None):
    """(local header + data, central record) for one entry at offset 0 (fixed up by archive())"""
    packed = data if method == 0 else zlib.compress(data)[2:-4]
    c = zlib.crc32(data) & 0xFFFFFFFF if crc is None else crc
    us = len(data) if size is None else size
    local = struct.pack('<IHHHHHIIIHH', 0x04034B50, 20, flags, method, 0, 0x21, c, len(packed), us, len(name), 0) + name + packed
    central = struct.pack('<IHHHHHHIIIHHHHHII', 0x02014B50, 20, 20, flags, method, 0, 0x21, c, len(packed), us, len(name), 0, 0, 0, 0, 0, 0) + name
    return local, central


def archive(entries):
    body, cds = b'', b''
    for local, central in entries:
        central = central[:42] + struct.pack('<I', len(body)) + central[46:]
        body += local
        cds += central
    return body + cds + struct.pack('<IHHHHIIH', 0x06054B50, 0, 0, len(entries), len(entries), len(cds), len(body), 0)


def hostile():
    ok = entry(b'teams.csv', b'team\n')
    return [
        ('a name climbing out', archive([ok, entry(b'../evil.txt', b'x')]), 'a zip archive with a file named'),
        ('a name deep climbing out', archive([entry(b'art/../../evil.txt', b'x')]), 'a zip archive with a file named'),
        ('an absolute name', archive([entry(b'/etc/passwd', b'x')]), 'a zip archive with a file named'),
        ('a backslash', archive([entry(b'art\\x.png', b'x')]), 'a zip archive with a file named'),
        ('a drive', archive([entry(b'C:x', b'x')]), 'a zip archive with a file named'),
        ('a file twice', archive([ok, entry(b'teams.csv', b'again\n')]), 'a zip archive with a file twice'),
        ('encrypted', archive([entry(b'teams.csv', b'team\n', flags=1)]), 'a zip archive with an encrypted file'),
        ('bzip2', archive([entry(b'teams.csv', b'team\n', method=12)]), 'a zip archive packed some other way'),
        ('too big unpacked', archive([entry(b'teams.csv', b'team\n', method=8, size=0x7FFFFFFF)]), 'a zip archive too big unpacked'),
        ('a wrong CRC', archive([entry(b'teams.csv', b'team\n', crc=1234)]), ''),       # (opens; reading it fails)
        ('not a zip', b'PK\x03\x04 but nothing else of one', 'not a zip archive'),
    ]


def c_bytes(b):
    return '(const unsigned char[]){ %s }' % ','.join(str(x) for x in b) if b else '(const unsigned char[]){ 0 }'


def main():
    print('/* zip archives for tests/zip_unit.c, made by tools/gen_zip_cases.py: Python\'s zipfile\'s, with their')
    print(' * files\' FNV-1a hashes; and hostile ones made by hand, with the refusal each must give ("": it opens,')
    print(' * but reading its file fails). */')
    print('struct zip_file { const char *name; unsigned size; unsigned long long hash; };')
    print('static const struct zip_case { const char *what; const unsigned char *zip; unsigned size; int files; struct zip_file file[8]; } zip_good[] = {')
    for what, z, files in good():
        fs = ', '.join('{ "%s", %d, 0x%016XULL }' % (n, len(b), fnv(b)) for n, b in sorted(files.items()))
        print('    { "%s", %s, %d, %d, { %s } },' % (what, c_bytes(z), len(z), len(files), fs))
    print('};')
    print('static const struct zip_bad { const char *what; const unsigned char *zip; unsigned size; const char *says; } zip_bad[] = {')
    for what, z, says in hostile():
        print('    { "%s", %s, %d, "%s" },' % (what, c_bytes(z), len(z), says))
    print('};')


if __name__ == '__main__':
    main()
