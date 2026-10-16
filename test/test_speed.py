"""Compile the actual speed-test function against host file calls, without a PS3 SDK."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'source/ata.c').read_text()
start = source.index('int file_speed_test(')
end = source.index('\nint drive_speed_test(', start)
with tempfile.TemporaryDirectory(prefix='ps3-speed-check-') as directory:
    build = Path(directory)
    (build / 'speed_impl.h').write_text(source[start:end])
    for flags in (['-O2'], ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']):
        binary = build / 'speed_check'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', *flags, '-I', str(build),
            str(root / 'test/speed_check.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
