# Contributing

Issues and pull requests are welcome. Report a security problem privately, as
[SECURITY.md](SECURITY.md) describes, never in a public issue.

## The most useful thing you can do first

Run it on your own hardware and file a **Hardware report** (the issue template asks the right
questions). The results recorded so far come from one Apple Silicon Mac and one Windows PC, where
depth and IR dropped about half their frames on a chipset USB controller. Reports from other USB
controllers, other Kinect adapters, and a physical iPhone against either host help most.

## Building and running the tests

Both hosts link libfreenect2, so even the synthetic tests need it installed first: follow the
README's [Setup](README.md#setup) for your platform.

**Windows** ([docs/windows-setup.md](docs/windows-setup.md) has the prerequisites):

```bat
windows\scripts\build.cmd test                                     & rem build, then the whole CTest suite
ctest --test-dir build\windows -R stream_e2e --output-on-failure   & rem one test, by name
```

The suite needs FFmpeg on the machine. Every test that starts the viewer or the CLI keeps
its state under `build\windows\windows\test-state\`.

**Mac and iPhone:**

```sh
xcodebuild -project KinectViewer.xcodeproj -scheme KinectHostApp \
  -destination 'platform=macOS,arch=arm64' -derivedDataPath build/DerivedData test
xcodebuild -project KinectViewer.xcodeproj -scheme KinectPocketViewer \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' -derivedDataPath build/DerivedData test
```

`kinect-cli` has synthetic self-checks of its own: `--synthetic --verify-barcode`,
`--test-recording`, `--test-hardening --synthetic` and `--verify-lock`.

`KinectViewer.xcodeproj` is generated from `project.yml` and committed. Change `project.yml`, run
`xcodegen generate`, and commit both: `xcodegen generate` on a clean checkout should give no diff.

## The rules

A few rules this codebase keeps, and why, so a change doesn't have to rediscover them:

- **Synthetic data only, never real frames.** This is a camera that sits inside someone's home.
  Tests, fixtures and screenshots use the synthetic source (`core/src/synthetic_source.cpp`) or
  made-up data. Nothing in development opens, decodes, screenshots or describes a real colour,
  depth or IR frame, live or recorded, or opens a real camera roll. The code holds the line too:
  under XCTest the Mac host starts on the synthetic source and its recorder refuses the real one;
  the Windows viewer's screenshot hook refuses the real source, any view with colour and a replay;
  a `--synthetic` or test-hook launch keeps its state in a temporary folder; a test camera roll
  refuses any frame that isn't synthetic; and the outside-the-home tests use a fake router on
  loopback. Keep it that way.
- **Images are synthetic too.** No picture of a real room, person, pet or home goes into the
  repository, an issue or a pull request.
- **No silent fallback.** A depth pipeline that fails to start stops the stream with its error. It
  never tries another.
- **Colour is live only.** Colour is never written to disk, with one exception: the Windows camera
  roll's own writer, thumbnailer and index, into the camera-roll folder. `Recorder::canRecord`,
  `HostRecorder.canRecord` and their refusals stay as they are.
- **The protocol only grows.** v1 and v2 peers must keep working against a newer host. A change to
  the wire goes into the protocol documents, `shared/WireProtocol.swift` and
  `windows/host/wire_protocol.{hpp,cpp}` in the same pull request. If code and document disagree,
  fix one of them there and say which.
- **Golden vectors are generated.** `windows/tests/golden/golden_vectors.inc` is the output of
  `python -I windows/tests/golden/derive_vectors.py`; edit the script, never the `.inc`.
- **Bounded before allocated.** Every length read from the network is capped before anything is
  allocated for it.
- **Fail closed.** A state file that can't be read never leads to a new identity, privacy mode off
  or a device let in. See [docs/pet-camera.md](docs/pet-camera.md) for each file's rule.
- **No weakening.** A test isn't changed to pass; the code is. Renaming a test is fine; its
  assertions stay. A new test should fail without the change it pins (try it).
- **No new warnings** on either platform, and no new dependency without a reason in the pull
  request and an entry in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- **Plain words, British spelling** (colour, licence) in the docs and in what the apps say.

## Open, and not yet done

These are known gaps. Each would make a good pull request.

- **CI.** There is none yet. libfreenect2 is linked even by the synthetic and protocol tests, so CI
  first needs a stub libfreenect2 target (or a cached build), then the Windows synthetic and
  protocol tests and the Swift tests on macOS.
- **Cross-checking Swift and C++.** The wire protocol is written twice. The Windows host's golden
  vectors were derived in Python by reading the Swift source, not by running it. The Swift tests
  should produce or verify the same vectors.
- **The Mac host at Windows' level.** Its pairing listener has no local-source check and no cap on
  connections, it reads frames of up to 4 MiB from peers that haven't paired, and it is ad-hoc
  signed without the hardened runtime. [SECURITY.md](SECURITY.md) lists these as known limits.
