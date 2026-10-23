"""Build, test and optionally deploy Deribit Gamma. No exchange access."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--qt', type=Path, default=Path(os.environ.get('QT_ROOT', root / '.deps/Qt/6.8.3/msvc2022_64')))
parser.add_argument('--skip-deploy', action='store_true')
parser.add_argument('--skip-configure', action='store_true')
parser.add_argument('--skip-tests', action='store_true', help='Compile only; run CTest separately with localhost access')
parser.add_argument('--asan', action='store_true')
parser.add_argument('--deploy-dir', type=Path, default=root / 'gamma-dist')
args = parser.parse_args()
env = {key.upper(): value for key, value in os.environ.items()}
env['PATH'] = str(args.qt / 'bin') + os.pathsep + env.get('PATH', '')
env['MSBUILDDISABLENODEREUSE'] = '1'
vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = None
if vswhere.exists():
    found = subprocess.run([str(vswhere), '-latest', '-products', '*', '-version', '[17.0,18.0)', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], capture_output=True, text=True, check=True)
    if found.stdout.strip():
        vs = Path(found.stdout.strip())
cmake = shutil.which('cmake', path=env['PATH'])
if not cmake and vs:
    cmake = str(vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
if not cmake or not Path(cmake).exists():
    raise SystemExit('Install Visual Studio 2022 C++ and CMake tools.')
if not (args.qt / 'lib/cmake/Qt6/Qt6Config.cmake').exists():
    raise SystemExit('Qt not found; provide --qt PATH.')
build = root / 'build' / ('gamma-asan' if args.asan else 'gamma')
test_temp = root / 'build' / 'test-temp'
test_temp.mkdir(parents=True, exist_ok=True)
env['TEMP'] = env['TMP'] = str(test_temp)
def run(command):
    subprocess.run([str(item) for item in command], cwd=root, env=env, check=True)
if not args.skip_configure:
    command = [cmake, '-S', root, '-B', build, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DCMAKE_PREFIX_PATH=' + str(args.qt.resolve()), '-DBUILD_TESTING=ON']
    if args.asan:
        command += ['-DCMAKE_CXX_FLAGS=/fsanitize=address /Zi', '-DCMAKE_EXE_LINKER_FLAGS=/INCREMENTAL:NO /DEBUG']
        if vs:
            compilers = sorted((vs / 'VC/Tools/MSVC').glob('*/bin/Hostx64/x64'), key=lambda path: tuple(map(int, path.parts[-4].split('.'))))
            if compilers:
                env['PATH'] = str(compilers[-1]) + os.pathsep + env['PATH']
    run(command)
run([cmake, '--build', build, '--config', 'Release', '--parallel', '1', '--', '/nr:false'])
try:
    if not args.skip_tests:
        run([Path(cmake).with_name('ctest.exe'), '--test-dir', build, '-C', 'Release', '--output-on-failure'])
except subprocess.CalledProcessError:
    for name in ('client-tests.txt', 'runtime-tests.txt', 'performance-tests.txt'):
        report = build / name
        if report.exists():
            print(report.read_text(encoding='utf-8', errors='replace'), flush=True)
    raise
if not args.skip_deploy and not args.asan:
    if args.skip_tests:
        raise SystemExit('Deployment requires tests; omit --skip-tests.')
    destination = args.deploy_dir.resolve()
    run([cmake, '--install', build, '--config', 'Release', '--prefix', destination])
    executable = destination / 'DeribitGamma.exe'
    if hashlib.sha256(executable.read_bytes()).digest() != hashlib.sha256((build / 'Release/DeribitGamma.exe').read_bytes()).digest():
        raise SystemExit('Installed executable differs from tested build.')
    if vs:
        crt = sorted((vs / 'VC/Redist/MSVC').glob('*/x64/Microsoft.VC143.CRT'), key=lambda path: tuple(map(int, path.parent.parent.name.split('.'))))
        if crt:
            for dll in crt[-1].glob('*.dll'):
                shutil.copy2(dll, destination / dll.name)
    print('Application:', executable)
