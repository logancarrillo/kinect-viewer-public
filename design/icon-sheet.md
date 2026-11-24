# icon-sheet.png

A contact sheet of the app icons, rendered from `design/logo.svg` by `design/render-icons.swift`
(`swift design/render-icons.swift`, which also writes every icon). The sheet has no text, so it carries no
font. These are its tiles, left to right.

**Top card: the macOS AppIcon set** (rounded-square tile, 64/1024 inset, corner radius 0.225, drop shadow)

| Tile | Size | Used as |
|---|---|---|
| 1 | 1024×1024 | 512@2x |
| 2 | 512×512 | 256@2x and 512@1x |
| 3 | 256×256 | 128@2x and 256@1x |
| 4 | 128×128 | 128@1x |
| 5 | 64×64 | 32@2x |
| 6 | 32×32 | 32@1x and 16@2x |
| 7 | 16×16 | 16@1x |
| 8 | 16×16 at 10× pixel zoom | to check the smallest size |

**Bottom card: the iOS AppIcon variants and the in-app mark**

| Tile | Size | Notes |
|---|---|---|
| 1 | iOS Light, 1024×1024 | full-bleed gradient, 24-bit RGB, no alpha, no baked corners |
| 2 | iOS Dark, 1024×1024 | mark on near-black, 24-bit RGB |
| 3 | iOS Tinted, 1024×1024 | greyscale luminance, 24-bit RGB |
| 4–6 | In-app LogoMark | the iPhone pairing screen and the Mac About box |
