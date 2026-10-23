"""Check the supported Qt build dependencies without changing runtime profiles."""
import os
from pathlib import Path
root = Path(__file__).resolve().parents[1]
qt = Path(os.environ.get('QT_ROOT', root / '.deps/Qt/6.8.3/msvc2022_64'))
if (qt / 'lib/cmake/Qt6/Qt6Config.cmake').exists():
    print('Qt found:', qt)
else:
    print('Install the official Qt 6.8.3 MSVC x64 build, including QtWebSockets.')
    print('With aqtinstall from PyPI:')
    print('  python -m pip install aqtinstall')
    print('  python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtwebsockets -O .deps/Qt')
    print('Then run scripts/build.py --qt PATH/TO/msvc2022_64')
