import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ISCC = os.environ.get('SCREENLIGHT_ISCC', 'D:/toolchains/InnoSetup7/ISCC.exe')
VERSION = '0.1.0'
OUT = os.path.join(HERE, 'out', 'release')


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    subprocess.check_call([sys.executable, os.path.join(HERE, 'build.py'), os.path.join('out', 'release')])
    exe = os.path.join(OUT, 'ScreenLight.exe')
    subprocess.check_call([ISCC, '/Q', os.path.join(HERE, 'installer', 'screenlight.iss')])
    portable = os.path.join(OUT, f'ScreenLight-{VERSION}-portable.zip')
    with zipfile.ZipFile(portable, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(exe, 'ScreenLight.exe')
        z.writestr('ScreenLight.ini', '\ufeff[Settings]\r\n'.encode('utf-16-le'))
    for name in sorted(os.listdir(OUT)):
        print(name, os.path.getsize(os.path.join(OUT, name)))


if __name__ == '__main__':
    main()
