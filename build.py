import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
MINGW = os.environ.get('SCREENLIGHT_MINGW', 'D:/toolchains/llvm-mingw-20261020-msvcrt-x86_64')
SRC = os.path.join(HERE, 'src')
OUT = os.path.join(HERE, 'out')
WIN7 = ['-Wl,--major-subsystem-version,6', '-Wl,--minor-subsystem-version,1', '-Wl,--major-os-version,6',
        '-Wl,--minor-os-version,1']
LIBS = ['-lcomctl32', '-luxtheme', '-lwtsapi32', '-ladvapi32', '-lshell32', '-lole32', '-ltdh']


def build():
    os.makedirs(OUT, exist_ok=True)
    res = os.path.join(OUT, 'screenlight.res.o')
    subprocess.check_call([f'{MINGW}/bin/llvm-windres.exe', '-I', SRC, '-O', 'coff', os.path.join(SRC, 'screenlight.rc'), res])
    sources = sorted(os.path.join(SRC, name) for name in os.listdir(SRC) if name.endswith('.cpp'))
    exe = os.path.join(OUT, 'ScreenLight.exe')
    subprocess.check_call([f'{MINGW}/bin/clang++.exe', '-std=c++20', '-O2', '-Wall', '-Wextra', '-municode', '-mwindows',
                           *sources, res, '-o', exe, '-static', '-Wl,--gc-sections', *LIBS, *WIN7])
    print(exe)


if __name__ == '__main__':
    build()
