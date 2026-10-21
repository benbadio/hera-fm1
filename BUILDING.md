# Building HERA

The build makes these files in `build/`:

| File | What |
| --- | --- |
| `hera.fwsc` | the installable package (app + update loader) |
| `hera.bin` | the firmware app |
| `hera.dis` | the disassembly, for the size and safety checks |

You don't need to build anything to use HERA: install it from the [installer](https://benbadio.github.io/hera-fm1/install.html). This page is for building it yourself.

## The easy way: GitHub

Fork the repository and push. GitHub Actions (`.github/workflows/build.yml`, Ubuntu) runs the tests, builds `hera.fwsc` and the Workbench, and publishes the Workbench to GitHub Pages. The package and the Workbench are attached to each run as artifacts (`hera-firmware`, `hera-workbench`).

## Prerequisites

- **Linux x86-64**: the toolchain runs natively.
- **macOS**: Docker Desktop. The JieLi toolchain is Linux x86-64 only, so each tool runs in a `linux/amd64` container. Keep the source in a folder Docker can share, e.g. under `/Users`.
- Python 3 with Pillow: `pip3 install Pillow`
- The JieLi toolchain (clang for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this repository.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations (`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`). The version and package identity come from the `VERSION` file (see below).

## Install from the command line

```
python3 tools/fm1_install.py build/hera.fwsc
python3 tools/fm1_install.py --info       # shows the identity of the connected FM-1
```

Or use the web [installer](https://benbadio.github.io/hera-fm1/install.html). If an install is cut off, hold **OCT −** while switching on (USB rescue) and install again.

## Tests

They run on macOS or Linux, with no hardware:

```
CC=clang sh tests/j6/run_tests.sh     # the engine: chords, phrases, sequencer, synth, glue (about 800,000 checks)
sh tests/run_hera_host.sh             # the whole firmware in a simulator: a PNG of every page and a WAV, in build/hera/
```

The host simulator is also how the screenshots in `docs/screens/` are made.

## The Workbench

The Workbench is the same firmware compiled to WebAssembly.

```
brew install llvm lld                 # macOS (on Linux: clang and lld)
sh web/build.sh                       # writes web/hera.wasm and web/hera_ids.js
python3 -m http.server --directory web
```

Then open <http://localhost:8000>. (Opening `index.html` from disk doesn't work: browsers won't load WebAssembly from a file path.)

## Versions and releases

- The version lives in the `VERSION` file (`0.1`, or `X.Y.Z`). The package identity follows it: `FM-1_43` + X + YY + ZZ, so 0.1 is `FM-1_4300100`, 0.1.1 is `FM-1_4300101` and 1.0 is `FM-1_4310000`. Every version change changes the identity.
- To release:
  1. Change `VERSION`.
  2. Add the version to the *Version history* in [HERA.md](HERA.md): what's new and fixed in the firmware, and (if it matters to users) in the Workbench.
  3. Commit, then tag and push: `git tag v0.2 && git push origin v0.2`. The tag has to match `VERSION`.
- `.github/workflows/release.yml` then builds the package on GitHub with the Firmware Hub's shared workflow, signs a build attestation, and attaches `hera-X.Y.fwsc` to the release. It also fills in the release notes from the version history and attaches the Workbench as a zip. Don't replace or add the `.fwsc` by hand afterwards: the hub compares its checksum with the signed one.
- Check a package yourself with `gh attestation verify hera-0.1.fwsc --repo benbadio/hera-fm1 --signer-workflow zvenson/fm1-build/.github/workflows/build.yml`.
