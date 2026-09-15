"""Optional post-build check: identical native payloads, ARM64 and 16 KB ELF alignment."""
import hashlib
from pathlib import Path
import struct
import zipfile
from common import b

reference = None
expected = {'libskate3.so', 'librexruntime.so', 'libc++_shared.so',
            'libhook_impl.so', 'libmain_hook.so'}
for variant in ('release', 'qa', 'debug', 'review'):
    apk = b / f'android/app/build/outputs/apk/{variant}/app-{variant}.apk'
    hashes = {}
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        assert not any(n.startswith('assets/') and n.lower().endswith(
            ('.xex', '.xexp', '.iso', '.big')) for n in names), 'retail game file in APK'
        for name in names:
            if not name.startswith('lib/') or not name.endswith('.so'):
                continue
            assert name.startswith('lib/arm64-v8a/'), name
            data = archive.read(name)
            assert data[:6] == b'\x7fELF\x02\x01', name
            assert struct.unpack_from('<H', data, 18)[0] == 183, name
            offset = struct.unpack_from('<Q', data, 32)[0]
            stride, count = struct.unpack_from('<HH', data, 54)
            loads = 0
            for i in range(count):
                segment = offset + i * stride
                if struct.unpack_from('<I', data, segment)[0] == 1:
                    alignment = struct.unpack_from('<Q', data, segment + 48)[0]
                    assert alignment >= 16384, (name, alignment)
                    loads += 1
            assert loads, name
            hashes[Path(name).name] = hashlib.sha256(data).hexdigest()
        assert hashes.keys() == expected, hashes.keys()
        assert any(n.startswith('assets/mods/') for n in names), 'bundled mod missing'
    if reference is None:
        reference = hashes
    assert hashes == reference, f'{variant} differs from the tested native payload'
    print(f'PASS: {variant}: five matching ARM64 libraries, 16 KB ELF alignment, bundled mod, no retail game files')
