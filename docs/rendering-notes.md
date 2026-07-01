# EarthView rendering notes — tile selection & depth on a 3D display

Non-obvious decisions behind `tiles_common` + the macOS shell, and the bugs
they fix. Read alongside PRD §6 and `docs/M1_KICKOFF.md`.

## 1. Tile selection uses ONE mono symmetric camera

Tile selection feeds cesium-native a **single mono `ViewState` with a symmetric
frustum** built from the head camera (`g_geoNav.cam`) — NOT the off-axis
per-eye `XrFovf`, not a recentered direction, not a dual coverage frustum.

This is exactly what `cesium-unity` does
(`native~/src/Runtime/CameraManager.cpp::unityCameraToViewState`) and what the
reference stereo Unity app (`LeiaInc/UnityCesiumGoogleTiles`) relies on: even for a
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

## 5. Two rigs: FLY = camera rig, FOCUS/orbit = display rig

EarthView submits poses through **`XR_EXT_view_rig`** (the runtime owns the
off-axis eyes). There are two rig parameterizations and the app switches between
them; **the world (cesium tiles) is always mapped camera-centric**, anchored at
the **XR origin**, via `xrFromEcefCamera(g_geoNav.cam, origin, s)` with
`s = kTargetXrDist / targetDist`. Both rigs only change how the runtime places
the eyes; **neither rig moves the world anchor.**

- **FLY → camera rig** (`XrCameraRigEXT`): a plain perspective camera at the XR
  origin. Identity pose; the runtime perturbs the *tracked* eyes around it.
  **Never anchor the world to the tracked-eye centroid** — that re-introduced
  off-centre / zoom-on-rotate on a real tracked panel. The cam rig owns the
  stereo via `verticalFov` + `convergenceDiopters`.
- **FOCUS/orbit → display rig** (`XrDisplayRigEXT`): a portal locked to the
  physical display. On a double-click the app converts the *live* camera rig →
  display rig with `dxr_view_rig_camera_to_display` so the switch is
  **disturbance-free** (the converter is exact — monkey-tested; do not "fix" it).

### The FOV coupling — read before touching `verticalFov`

The runtime rig math has a fixed reference half-tangent
`kCameraHalfTanVfov = 0.32492` (= `tan(18°)`, a 36° vFOV) in
`displayxr-common/common/rig_mode.cpp`. `verticalFov` and `zoomFactor` are a
**coupled pair**: `cameraRig.verticalFov = 2·atan(kCameraHalfTanVfov/zoomFactor)`.
So the three cam-rig FOV sites MUST agree on the same effective FOV:

1. `cameraRig.verticalFov` (fly render),
2. `crig0.half_tan_vfov` — the **source** FOV fed to the focus converter; it
   MUST equal the live cam rig, or the cam↔display switch pops, **and**
3. the tile-**selection** frustum vfov (must match the render FOV — §2).

We render fly at the **display's physical FOV** (orthoscopic):
`2·atan(displayHeightM / (2·nominalZ))` — the `CamVFovRad()` helper. Use the
**full display height**, NOT the window canvas, so a small window keeps the
full-screen framing (canvas-based FOV goes uncomfortably narrow when windowed).
A physical FOV is `zoomFactor ≈ 2` here → `perspective_factor == 1` (full
perspective) in the converter.

### What the converter's pose offset is (and is NOT)

`dxr_view_rig_camera_to_display` sets `out->pose.position = in->pose + R·(0,0,
-es·N)` where **`es·N == 1/invd` = the convergence distance** — and it is
**independent of FOV** (`half_tan_vfov` cancels in `es = persp·vH/H`). This
offset is applied by the runtime to the *located eyes*; it is **NOT** a shift of
the cesium world anchor and **NOT** caused by `zoomFactor`. Anchoring the world
at `displayRig.pose.position` shoves the view back by the whole convergence
distance ("sends me way back") — don't. Keep the world anchored at the origin
in both modes (§3, §6).

## 6. Zoom is a CESIUM-WORLD operation, not a rig operation

Zoom (scroll/dolly) does **not** touch the rig. It scales the **tile world** via
`targetDist` → `s` (`xrFromEcefCamera`). So zoom must keep the orbit pivot fixed
*itself* — the rig won't do it.

In FLY, `GeoNav::dolly()` (forward, toward the geo target) is correct. In
**FOCUS/orbit it is NOT**: the geo target (`cam.pos + cam.dir·targetDist`) is not
the orbit POI during orbit (`targetDist = radius/vDist ≠ radius`), and the
convergence-plane re-pin only runs on **drag**. So a plain dolly slides the POI
off the convergence plane and the zoom centre drifts off the pivot.

This drift existed all along but the old canonical 36° FOV is *weak* perspective
(`perspective_factor ≈ 0.5`), which hid it; the physical FOV is full perspective
(`1.0`), which made it obvious — i.e. it was a **latent** bug surfaced by §5, not
caused by it.

**Fix:** focus-mode zoom is a **radius change about the POI** — scale
`v = cam.pos − POI` by `pow(0.9, steps)`, then re-pin
`targetDist = |v| / vDist` (the *same* re-pin the orbit drag uses) so the POI
stays centred and at zero parallax. See `UpdateGeoNav`, the dolly block.
