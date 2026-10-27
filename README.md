Tested myself; works as intended. Fully AI-generated in 2 days; be careful.

UI looks similar to Grid-bot ones.

# Deribit Gamma Scalper

Windows x64 app that hedges existing BTC or ETH options with USDC perpetuals. It does not buy or sell options; positive net gamma is required to hedge.

## Download and run

1. Open [Releases](https://github.com/nieledran/Deribit_Gamma_Scalping_Delta_Escape_approach/releases) and download the **Windows x64 ZIP** under **Assets**. The GitHub **Source code** ZIP needs building (below).
2. Right-click the ZIP → **Extract All**. Keep all extracted files together in a writable folder.
3. Open that folder and double-click **start.cmd**. No Python, Qt or compiler installation is needed for the release.
4. Choose the instrument and **Testnet** or **Production**, then connect with that environment's Deribit API key (account read and trading access).
5. Select existing option positions, click **Apply**, set **Delta coverage (%)** (100% offsets the options' delta) and **Delta Change** (delta tolerance), then click **Start / Resume**. The app starts paused.

**Stop & Cancel** cancels hedge orders and leaves positions open. **Close hedge** closes only the perpetual hedge using passive limit orders; filling can take time. Do not manually trade the selected construction while the bot manages it.

Settings and history are in `gamma-profiles/` beside the executable. Keep this folder when updating; close the app before replacing files or copying its databases. API credentials stay in memory.

## Build from source (optional)

If no Windows release ZIP is available:

1. Install **Python 3** (enable **Add python.exe to PATH**) and **Visual Studio 2022 Build Tools** with **Desktop development with C++**, including CMake tools and a Windows SDK.
2. Download the repository using **Code → Download ZIP** and extract it.
3. Open the extracted folder in PowerShell (**Open in Terminal**) and run:

```powershell
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtwebsockets -O .deps/Qt
python scripts/build.py --skip-deploy
& .\.deps\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --compiler-runtime .\build\gamma\Release\DeribitGamma.exe
.\build\gamma\Release\DeribitGamma.exe
```

Continue from step 4 above. The build runs offline tests before you launch the app. More build options: [docs/BUILDING.md](docs/BUILDING.md).

MIT license: [LICENSE](LICENSE). Dependency notices: [THIRD_PARTY.md](THIRD_PARTY.md).
