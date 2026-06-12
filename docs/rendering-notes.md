# EarthView rendering notes — tile selection & depth on a 3D display

Non-obvious decisions behind `tiles_common` + the macOS shell, and the bugs
they fix. Read alongside PRD §6 and `docs/M1_KICKOFF.md`.

## 1. Tile selection uses ONE mono symmetric camera

Tile selection feeds cesium-native a **single mono `ViewState` with a symmetric
frustum** built from the head camera (`g_geoNav.cam`) — NOT the off-axis
per-eye `XrFovf`, not a recentered direction, not a dual coverage frustum.

This is exactly what `cesium-unity` does
(`native~/src/Runtime/CameraManager.cpp::unityCameraToViewState`) and what the
Leia stereo Unity app (`LeiaInc/UnityCesiumGoogleTiles`) relies on: even for a
3D display, selection is mono. **The off-axis Kooima stereo is a render-time
projection concern only — it never enters tile selection.**

Trying to reconstruct the asymmetric per-eye frustum for selection was a dead
end: it regressed coverage and, with a wide "coverage" frustum bolted on to
compensate, ballooned to ~1400 drawn tiles / 1.3 GB. The mono path holds
~50–100 drawn / ~400–600 MB.

`TilesetOptions` that matter at oblique/low-altitude views:
`enableFogCulling = false` (globe-tuned slant-distance culling drops
oblique-view tiles) and `forbidHoles = true` (don't show a half-refined region).

## 2. The selection frustum must CONTAIN the off-axis display frustum

The viewer eye sits **above** the display centre, so the display subtends an
**asymmetric** angle from the eye: its bottom edge is farther below the eye's
forward axis than the top edge is above it (`|angleDown| > angleUp`).

cesium's `ViewState` is symmetric, so the smallest symmetric frustum that
*contains* the off-axis display frustum centres on the eye's **forward** axis
with half-angles:

```
vHalf = max(|angleDown|, |angleUp|)   // not (angleUp - angleDown)/2
hHalf = max(|angleLeft|, |angleRight|)
```

(+15 % margin). Sizing to `(up - down)` or centring on the gaze-to-display-centre
under-covers the bottom — that was the *"tiles missing / clipped looking down"*
selection symptom. See `macos/main.mm`, the `selCam` block. (vfov ≈ 17° → 35°.)

## 3. Near plane must scale with the world, not the display height

`xrFromEcef` maps the ECEF scene into XR space with a scale `s` chosen so the
view target lands on the display plane (`stereoScaleForDistance`). At a typical
bookmark (`targetDist = 2500 m`) `s ≈ 0.0025`, which places the eye at
`ez ≈ 6.3` XR-metres from the display.

The render near/far were inherited from modelviewer as `near = ez - rigVH`
(`rigVH = virtualDisplayHeight / scaleFactor ≈ 1.5`), which assumes content is
framed to ~`rigVH` around the display. EarthView's streamed ground is **not**
framed that way — the foreground (window bottom) pops out close to the eye. At
startup it sat ~2.9 m from the eye while the near plane was at
`6.3 − 1.5 = 4.8 m`, so the whole bottom strip was **near-clipped to sky**.

That is why the bug was scale-dependent and vanished after an orbit round-trip:
`releaseToFly` leaves a different `targetDist` → larger `s` → the foreground
maps farther from the eye, back past the stale near plane.

Fix (`macos/main.mm`, the per-eye near/far): put the near plane just in front of
the eye, scaled to `ez` so it is robust to `s`:

```
near_z = max(ez * 0.01f, 1e-4f);   // was (ez - rigVH)
far_z  = ez + 1000 * rigVH;        // unchanged
```

## 4. Self-verification (no user screenshot, no TCC)

`xrCaptureAtlasEXT` is unreliable on macOS vk_native and `screencapture` needs
TCC the agent lacks. `TileRenderer::dumpColorTarget()` copies the internal
colour target → `stbi_write_png`; the frame loop fires it once via
**`DXR_DUMP=N`** → `/tmp/earthview_dump.png` (mono eye 0 — clearer than an
anaglyph grab). Pair it with the per-frame `tiles:` log line (`s`, `targetDist`,
alt, drawn/skip counts) — dump + numbers is what pinned the near-plane clip in
one shot. `EV_ELEV` / `EV_DIST` force a bookmark framing to reproduce a pose.
See `CLAUDE.md` → "Self-capture".
