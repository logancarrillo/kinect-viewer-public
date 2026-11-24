**What changed, and why**

**How it was tested**
- [ ] Windows: `windows\scripts\build.cmd test` (name any test that fails, and why)
- [ ] Mac: `xcodebuild ... test` for `KinectHostApp` and `KinectPocketViewer`
- [ ] Tested on real hardware (say which), or synthetic only

**The rules** (from `CONTRIBUTING.md`)
- [ ] Tests and screenshots use the synthetic source or made-up data only: no real frame was
      opened, decoded, screenshotted or described, and no image of a real room, person or pet
      is attached
- [ ] No test was weakened or deleted; a new test fails without this change
- [ ] A wire change updates the protocol docs and both the Swift and the C++ side
- [ ] `project.yml` and `KinectViewer.xcodeproj` change together (`xcodegen generate` gives no diff)
- [ ] No new warnings; a new dependency is explained and in `THIRD_PARTY_NOTICES.md`

**What only a person with the hardware can check**
