# Building and packaging

Use Windows x64, Visual Studio 2022 C++/CMake tools, Python 3 and Qt 6.8.3 MSVC x64 with Core, Widgets, Network, WebSockets, SQL and Test. Python is a build tool only; runtime trading, persistence, analytics and UI are C++.

```powershell
python scripts/dependencies.py
python scripts/build.py --skip-deploy
python scripts/build.py
```

If a sandbox blocks local TCP sockets (Windows error 10013), compile with
`python scripts/build.py --skip-deploy --skip-tests`, then run CTest in an environment
that permits localhost. The WebSocket tests use local fake servers only. A passing
build never substitutes sandbox failures for successful validation; deployment
with `--skip-tests` is rejected.

The dependency command verifies Qt or prints its official installation command. Boost is no longer required. An existing installation can be supplied using `--qt C:/Qt/6.8.3/msvc2022_64` or `QT_ROOT`.

Build outputs use `build/gamma/`, separate from all old grid caches. `build.py` discovers Visual Studio/CMake, builds Release and runs all CTest scenarios. Deployment happens only after successful tests and writes `gamma-dist/`. Close any running gamma executable in that destination before replacing it; use `--deploy-dir build/public-runtime` for an independent runtime.

```powershell
python scripts/build.py --asan --skip-deploy
python scripts/build.py --deploy-dir build/public-runtime
python scripts/package.py --dist build/public-runtime
```

`--skip-configure` reuses the current gamma CMake configuration. ASan uses its own `build/gamma-asan` directory. The portable packager explicitly selects binaries/docs/licenses, verifies the x64 executable and Qt hashes, and includes Qt source archives, SBOMs and license texts. Profiles, SQLite/WAL files, credentials, exports and the legacy archive are excluded.

The executable is `DeribitGamma.exe`; its profiles are `gamma-profiles/btc-testnet.db`, `btc-production.db`, `eth-testnet.db`, or `eth-production.db`. An analytics database is stored next to each core profile. Do not copy just a live `.db` while its WAL exists: use SQLite's online backup or stop the application first.

Tests cover normalized delta, capped initial hedging, cooldown, expiry, margin validation, model/curve consistency, journal deduplication, settlement/funding, reporting baselines, persistence-before-send and trading with a blocked GUI event loop. All exchange mutation tests use loopback fake servers. No testnet or Production order is submitted automatically.

The UI regression feeds changing prices, P&L, latency, clock availability and bot states
through a 960 x 540 window and checks that field geometry stays fixed. CTest supplies
`QT_QPA_FONTDIR` from the Windows Fonts directory for offscreen UI/performance tests;
without it Qt's dummy font metrics do not represent the real Windows layout. Native
Windows captures and streaming checks are also run at 100%, 125% and 150% scaling.

The offline performance test writes `build/gamma/performance-report.json`. It compares
the original continuous report workload (full ledger replay plus delta curve and
escape search) against the cached report on 2,001 history rows, then feeds changing
synthetic snapshots to four 960 x 540 windows. GUI/event timings and process CPU are
recorded; this does not measure exchange execution speed.

For a second comparison of four static demo processes, preserve a prior runtime and run:

```powershell
python scripts/benchmark_windows.py --before PATH/TO/OLD/DeribitGamma.exe --after PATH/TO/NEW/DeribitGamma.exe
```

Preview cleanup with `python scripts/clean.py`. Only named reproducible build/cache
categories are eligible; add `--apply` to remove them. `legacy-archive`, release files,
active distributions, exports, test-temp and all profile directories are excluded.
Links and database/credential files inside a target prevent its deletion. Unused
Boost caches can be previewed separately with `--targets unused-boost`.

Profile-reset regression tests reproduce a restart after a manual perpetual change, an
unlocked Delete local profile action with a default-No P&L warning, cancellation by
exact owned labels, a late order appearing at the second verification, rejected
cancellation, transactional rollback, remaining external positions and analytics
recovery after an interrupted reset notification. They use temporary profiles and
loopback fake exchange servers only.
