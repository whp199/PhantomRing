# Contributing to PhantomRing

Thanks for helping. Bug reports, live-test results on hardware we don't have (AMD/Intel GPUs, Steam Deck, Windows),
reverse-engineering facts and pull requests are all welcome. Please read the rules below first. The legal ones are not
negotiable.

## The rules

**No game code or assets, ever.** Nothing from either game goes into the repository or a release: no executables, DLLs,
shaders, models, textures, sounds, regulation data, saves or `pr record` captures (`*.prrec`, which contain MGSV's assets).
Names, offsets, byte patterns and measured numbers are facts, and facts are fine (`tools/re/data/` is that kind of file).

**GPL sources are read for facts only.** tpp-mod (GPL-3) and any other GPL project may be *read* to learn facts: an
address, a structure offset, a function's name or role, a call order, what a value means. Its code must never be copied,
translated line by line or paraphrased into this repository, and that includes its comments and its structure. Write your
own code from your own understanding, verify the fact on the local game yourself, and if a GPL project led you to it,
name it in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) under "Facts only (no code)". If you're not sure whether
something counts as a fact, ask in the PR.

**Third-party code needs an MIT-compatible licence** (MIT, BSD, zlib, Apache-2.0 where compatible) and an entry in
THIRD-PARTY-NOTICES.md that says what was taken or adapted and from where. Adapting a design from SkyCraft (MIT) or
MGS5VR (MIT) is fine with that credit.

**Elden Ring stays offline.** PhantomRing always starts Elden Ring through Mod Engine 3 with `start_online = false`
and never loads Easy Anti-Cheat. Don't add anything that could take a modded game or a PhantomRing save online.
PhantomRing never accepts terms or answers Konami's region/Terms of Use prompt for the player.

**Every game address is byte-verified.** Each address or hook site is checked against its expected bytes before use. If
it doesn't match, that feature turns off and the log says why. The game must keep running normally. Follow the style of
`er/Addresses.cpp` and `mgsv/Verify.h`.

## Building and testing

The build needs MinGW-w64 GCC 13+, CMake 3.25+, Ninja and Python 3.11+. The tests also need wine (README "Building and
tests"):

```bash
git clone --recursive https://github.com/whp199/PhantomRing && cd PhantomRing
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
export WINEPREFIX=$(mktemp -d) PHANTOMRING_WINEPREFIX=$WINEPREFIX   # keeps the tests off your own prefix
python3 tools/phantomring/test_launcher.py && python3 tools/phantomring/test_fakes.py && python3 tools/phantomring/test_replay.py
cmake --build build --target check_host check_guest check_windows   # xvfb-run -a ... on a machine without a display
python3 tools/phantomring/test_layout.py && python3 tools/phantomring/test_plugins.py
python3 tools/phantomring/test_release.py
```

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs exactly these on every push and pull request. Its header
says how to reproduce a run. What each test needs is in [docs/TOOLS.md](docs/TOOLS.md#tests-offline). The checks that
need the games (`verify_er_addresses`, `mgsv/verify_addresses.py`, the regulation-backed combat rows) are run by hand on
a machine that has them.

**Protocol changes** ([protocol/phantomring_protocol.h](protocol/phantomring_protocol.h)) must be additive. Bump
`kVersion`, mirror the change in `tools/phantomring/proto.py` and `tests/layout_dump.cpp`, and `test_layout.py` must pass.
Two plugins that speak different protocol versions refuse to attach, and the player sees
"PhantomRing versions differ: ... -- reinstall both".

**Versions.** The [VERSION](VERSION) file is the only version number. CMake, both DLLs (which also embed the commit id),
`pr --version` and the packages' BUILD.txt all read it. To release, bump VERSION, commit, and push the tag `v<VERSION>`
(e.g. `v0.2.0-alpha`). CI builds and tests the tag, then publishes the Windows zip and the Linux tarball as a GitHub
pre-release with SHA256SUMS. A tag that doesn't match VERSION, or a build with uncommitted changes, is refused.

## Live-test etiquette

Offline tests prove the logic. Only the games prove that it works. When you test live:

- **Saves first.** `pr launch` backs up both games' saves before every start, and Elden Ring plays its own copy
  (`PhantomRing0000.sl2`). When you test on someone else's character, put their save back afterwards
  (`tools/pr restore-saves <time> --yes`), and don't kill bosses or spend their runes or items.
- **Offline only.** Never run a PhantomRing setup online, and never accept terms or agreements in either game for anyone.
- **One build at a time.** Several people or agents sharing a tree build through `flock build/.buildlock cmake --build
  build`. Don't run `check_host` while Elden Ring runs from that build directory, because it overwrites
  `build/PhantomRingHost.log`. And don't build while someone is playing: both games together use most of the machine.
- **Machines with the displays off**: `tools/pr launch --er-headless 3840x2160` runs Elden Ring in its own headless
  gamescope. `pr er 'hud shot'` grabs a frame, and `pr er "input virtualpad on"` with presses drives it. Turn the
  virtual pad off when you're done.
- **Say what you saw.** Say what you verified live, and say what you only tested offline. The docs mark the difference
  (VERIFIED / UNVERIFIED in [docs/STATUS.md](docs/STATUS.md) and the per-plugin docs). Attach `tools/pr collect-logs`
  output to reports.
- **Clean up.** `tools/pr stop` when you're done. Nothing PhantomRing started should be left running.

## Pull requests

- One topic per PR. Explain what changed and how you tested it (offline suites, plus live if it touches the games).
- Update the docs that describe the behaviour you changed (README, docs/HOST.md, docs/GUEST.md, docs/TOOLS.md, ...).
- Keep the house style: tabs, `a_` parameters, short comments that say *why*, failures logged with what was found.
- By contributing you agree that your contribution is released under the project's [MIT License](LICENSE).
