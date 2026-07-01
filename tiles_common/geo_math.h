// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// geo_math — the EarthView coordinate model (PRD §6.1).
//
// 3D Tiles are ECEF (meters, ~6.4e6 magnitudes — unrenderable in float32).
// ALL doubles live here and in the per-frame draw-list build; the frame loop
// and GPU see only float[16]. Precision scheme (M0-proven, 0.0009 px):
// tile-local float vertices + a per-tile double matrix product
//   M_tile_d = xrFromEcef_d × yUpToZUp_d × Tile::getTransform()
// cast to float once per frame. The huge ECEF translations cancel inside the
// double product; anchor/bookmark changes never touch GPU memory.

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>

namespace geo {

// glTF Y-up -> ECEF Z-up: (x,y,z) -> (x,-z,y). cesium-native's tile transform
// does not bake this in; premultiply onto Tile::getTransform() (M0 fact 2).
glm::dmat4
yUpToZUp();

// Free camera in ECEF doubles. dir/up unit-length, orthogonal-ish (up is
// re-orthogonalized by viewMatrix()).
struct GeoCamera
{
	glm::dvec3 pos{0.0};
	glm::dvec3 dir{0.0, 0.0, -1.0};
	glm::dvec3 up{0.0, 0.0, 1.0};

	// ECEF -> camera-local (right-handed, looking down -Z). Double lookAt.
	glm::dmat4
	viewMatrix() const;
};

struct Bookmark
{
	const char *name;
	double lonDeg;
	double latDeg;
	double altM;     // target height above ellipsoid
	double distM;    // camera distance from target
	double azDeg;    // 0 = due north, +east
	double elevDeg;  // above local horizon
};

const Bookmark *
bookmarks(size_t *outCount);

// Orbit-framing camera for a bookmark (port of the M0 spike's buildCamera).
GeoCamera
cameraForBookmark(const Bookmark &b);

// ECEF position of a bookmark's target point.
glm::dvec3
bookmarkTarget(const Bookmark &b);

// Local ENU (east-north-up) frame at an ECEF point: columns = E,N,U, origin p.
// enuFromEcef() is its inverse (ECEF -> local tangent space at p).
glm::dmat4
enuFrame(const glm::dvec3 &pEcef);
glm::dmat4
enuFromEcef(const glm::dvec3 &pEcef);

// Height of an ECEF point above the WGS84 ellipsoid (meters, approximate is
// fine — drives the stereo-scale knob only).
double
heightAboveEllipsoid(const glm::dvec3 &pEcef);

// Distance (metres) from posEcef along dirEcef (unit) to the first WGS84
// ellipsoid (ground) intersection, or -1 if the ray misses / points away. Used
// to auto-focus the stereo convergence on the GROUND under the crosshair — a
// smooth surface, unlike a depth-buffer read which snags on buildings.
double
rayGroundDistanceM(const glm::dvec3 &posEcef, const glm::dvec3 &dirEcef);

// ── Camera-centric XR mapping (default view model, PRD §6.1) ─────────────
//
// Maps the full-scale ECEF world into XR space so the geo camera sits at the
// viewer position looking through the display: scene point
//   p_xr = viewerPosXr + s * cameraView(p_ecef).
// `s` (XR meters per geo meter) is the stereo scale: content at the camera's
// target distance lands on the display plane (ZDP), so parallax stays in
// budget at any altitude.
glm::dmat4
xrFromEcefCamera(const GeoCamera &cam, const glm::dvec3 &viewerPosXr, double s);

// Comfortable stereo scale: target distance maps to the viewer-to-display
// distance (zdpDist, from XR_EXT_display_info nominal viewer), clamped.
double
stereoScaleForDistance(double targetDistM, double zdpDistXr);

// ── Display-centric diorama (acquired view model, gauss pattern) ─────────
//
// Frames the neighborhood around `anchorEcef` as a tabletop diorama:
//   p_xr = centerXr + S(s) * ENU(anchor)^-1 (p_ecef)
// with ENU up mapped to XR +Y. s ~ 1/3000 (knob to ~1/8000), yawRad spins
// the diorama about its up axis.
glm::dmat4
xrFromEcefDiorama(const glm::dvec3 &anchorEcef,
                  const glm::dvec3 &centerXr,
                  double s,
                  double yawRad,
                  double tiltRad);

// Selection camera matching the diorama view: the XR viewer's axis mapped
// into geo space at the equivalent distance (|viewerPosXr| / s), so the
// selected footprint and SSE both track the tabletop zoom. (Selecting with
// the geo orbit camera instead shrinks the frustum to nothing as you zoom.)
GeoCamera
dioramaSelectionCamera(const glm::dvec3 &anchorEcef,
                       double s,
                       double yawRad,
                       double tiltRad,
                       const glm::dvec3 &viewerPosXr);

// ── Navigation state machine (PRD §7.1) ──────────────────────────────────
//
// Camera-centric free navigation with an optional acquired orbit center.
// All inputs are in radians / scroll steps; translation speeds scale with
// targetDist so street level and city overview feel the same.
struct GeoNav
{
	GeoCamera cam;
	double targetDist = 2500.0; // m to the point of interest (drives speeds + stereo scale)

	bool orbitAcquired = false;
	glm::dvec3 orbitCenter{0.0};
	// Display-centric diorama state (orbit-acquired). Seeded from the camera
	// pose at acquire time so the double-click transition preserves the
	// apparent view (no zoom/orientation jump); drag = spin + tilt, wheel =
	// tabletop scale. The geo camera does NOT drive the rig in this mode.
	double dioramaYaw = 0.0;  // spin about the table's up axis
	double dioramaTilt = 0.0; // tilt toward the viewer (0 = edge-on horizon)
	double dioramaScale = 1.0 / 3000.0;

	int bookmarkIndex = 0;

	void
	frameBookmark(int index); // jump-frame bookmark (fly-over interp = M1.x)
	void
	cycleBookmark();

	// Left-drag: free-look (camera-centric) or orbit around the acquired
	// center (diorama).
	void
	look(double dYawRad, double dPitchRad);
	// Right-drag / WASD: pan in the camera right/up plane (scaled by targetDist).
	void
	pan(double dxNorm, double dyNorm);
	// W/S along view dir.
	void
	dolly(double steps); // exponential: each step moves a fraction of targetDist
	// E/Q: climb/descend along the local up (scaled by targetDist).
	void
	elevate(double dyNorm);

	// Double-click pick result -> acquire orbit center (diorama mode).
	// zdpXr = viewer-to-display distance in XR meters (for scale continuity:
	// the diorama starts at the same apparent zoom the camera view had).
	void
	acquireOrbit(const glm::dvec3 &centerEcef, double zdpXr);
	// Esc/Space: release back to camera-centric, reframe current bookmark.
	void
	releaseOrbit();
	// 'C': release back to camera-centric CONTINUOUSLY — the free camera
	// takes over exactly where the diorama view was (no reframe jump).
	void
	releaseToFly(double zdpXr);
};

} // namespace geo
