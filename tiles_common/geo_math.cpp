// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "geo_math.h"

#include <CesiumGeospatial/Cartographic.h>
#include <CesiumGeospatial/Ellipsoid.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace geo {

namespace {

const CesiumGeospatial::Ellipsoid &
wgs84()
{
	return CesiumGeospatial::Ellipsoid::WGS84;
}

// Bookmark table (PRD §7.1). Paris/Eiffel default = the M0 spike framing.
const Bookmark kBookmarks[] = {
    {"Paris", 2.2945, 48.8584, 60.0, 2500.0, 158.0, 35.0},
    {"San Francisco", -122.4783, 37.8199, 70.0, 2500.0, 245.0, 30.0}, // Golden Gate
    {"New York", -73.9857, 40.7484, 100.0, 2200.0, 200.0, 30.0},      // Midtown
    {"Tokyo", 139.8107, 35.7101, 150.0, 2500.0, 225.0, 30.0},         // Skytree
    {"Sydney", 151.2153, -33.8568, 30.0, 1800.0, 135.0, 30.0},        // Opera House
};

// Rotation of the diorama mapping: ENU -> XR table frame, then user spin
// (yaw about table up) and tilt (toward the viewer, about screen X). Shared
// by the forward transform, the selection camera, and table-frame panning so
// they can never drift apart.
glm::dmat3
dioramaRotation(double yawRad, double tiltRad)
{
	glm::dmat3 enuToXr(glm::dvec3(1, 0, 0),  // east  -> +X (columns)
	                   glm::dvec3(0, 0, -1), // north -> -Z
	                   glm::dvec3(0, 1, 0)); // up    -> +Y
	glm::dmat3 yaw = glm::dmat3(glm::rotate(glm::dmat4(1.0), yawRad, glm::dvec3(0, 1, 0)));
	glm::dmat3 tilt = glm::dmat3(glm::rotate(glm::dmat4(1.0), tiltRad, glm::dvec3(1, 0, 0)));
	return tilt * yaw * enuToXr;
}

} // namespace

glm::dmat4
yUpToZUp()
{
	glm::dmat4 m(1.0);
	m[1] = glm::dvec4(0, 0, 1, 0);
	m[2] = glm::dvec4(0, -1, 0, 0);
	return m;
}

glm::dmat4
GeoCamera::viewMatrix() const
{
	return glm::lookAt(pos, pos + dir, up);
}

const Bookmark *
bookmarks(size_t *outCount)
{
	if (outCount) {
		*outCount = sizeof(kBookmarks) / sizeof(kBookmarks[0]);
	}
	return kBookmarks;
}

glm::dvec3
bookmarkTarget(const Bookmark &b)
{
	auto carto = CesiumGeospatial::Cartographic::fromDegrees(b.lonDeg, b.latDeg, b.altM);
	return wgs84().cartographicToCartesian(carto);
}

GeoCamera
cameraForBookmark(const Bookmark &bIn)
{
	// Dev-only overrides for autonomous testing of specific framings
	// (EV_ELEV=75 reproduces "camera pointing steeply down").
	Bookmark b = bIn;
	if (const char *e = std::getenv("EV_ELEV")) {
		b.elevDeg = std::atof(e);
	}
	if (const char *e = std::getenv("EV_DIST")) {
		b.distM = std::atof(e);
	}
	auto carto = CesiumGeospatial::Cartographic::fromDegrees(b.lonDeg, b.latDeg, b.altM);
	glm::dvec3 target = wgs84().cartographicToCartesian(carto);
	glm::dvec3 up = wgs84().geodeticSurfaceNormal(carto);
	glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
	glm::dvec3 north = glm::normalize(glm::cross(up, east));
	double az = glm::radians(b.azDeg), el = glm::radians(b.elevDeg);
	glm::dvec3 horiz = glm::normalize(std::cos(az) * north + std::sin(az) * east);
	GeoCamera c;
	c.pos = target + horiz * (b.distM * std::cos(el)) + up * (b.distM * std::sin(el));
	c.dir = glm::normalize(target - c.pos);
	c.up = up;
	return c;
}

glm::dmat4
enuFrame(const glm::dvec3 &pEcef)
{
	glm::dvec3 up = glm::normalize(wgs84().geodeticSurfaceNormal(pEcef));
	glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
	glm::dvec3 north = glm::normalize(glm::cross(up, east));
	glm::dmat4 m(1.0);
	m[0] = glm::dvec4(east, 0.0);
	m[1] = glm::dvec4(north, 0.0);
	m[2] = glm::dvec4(up, 0.0);
	m[3] = glm::dvec4(pEcef, 1.0);
	return m;
}

glm::dmat4
enuFromEcef(const glm::dvec3 &pEcef)
{
	// The frame is orthonormal — invert via transpose-rotation + translation.
	return glm::inverse(enuFrame(pEcef));
}

double
heightAboveEllipsoid(const glm::dvec3 &pEcef)
{
	auto carto = wgs84().cartesianToCartographic(pEcef);
	return carto ? carto->height : 0.0;
}

glm::dmat4
xrFromEcefCamera(const GeoCamera &cam, const glm::dvec3 &viewerPosXr, double s)
{
	// p_xr = viewerPos + s * (camera-local p). Camera-local already looks down
	// -Z, matching the XR viewer-facing-display convention.
	glm::dmat4 t = glm::translate(glm::dmat4(1.0), viewerPosXr);
	glm::dmat4 sc = glm::scale(glm::dmat4(1.0), glm::dvec3(s));
	return t * sc * cam.viewMatrix();
}

double
stereoScaleForDistance(double targetDistM, double zdpDistXr)
{
	double d = std::max(targetDistM, 1.0);
	double s = zdpDistXr / d;
	// Clamp: never blow city blocks past life-size, never shrink below
	// 1:200000 (whole-region overview keeps a hint of depth).
	return std::clamp(s, 1.0 / 200000.0, 1.0);
}

glm::dmat4
xrFromEcefDiorama(const glm::dvec3 &anchorEcef,
                  const glm::dvec3 &centerXr,
                  double s,
                  double yawRad,
                  double tiltRad)
{
	glm::dmat4 rot = glm::dmat4(dioramaRotation(yawRad, tiltRad));
	glm::dmat4 t = glm::translate(glm::dmat4(1.0), centerXr);
	glm::dmat4 sc = glm::scale(glm::dmat4(1.0), glm::dvec3(s));
	return t * rot * sc * enuFromEcef(anchorEcef);
}

GeoCamera
dioramaSelectionCamera(const glm::dvec3 &anchorEcef,
                       double s,
                       double yawRad,
                       double tiltRad,
                       const glm::dvec3 &viewerPosXr)
{
	glm::dmat3 rot = dioramaRotation(yawRad, tiltRad);
	glm::dmat3 enuRot = glm::dmat3(enuFrame(anchorEcef)); // ENU -> ECEF rotation

	// Selection axis = the viewer's -Z (same convention as the camera-centric
	// model), NOT the viewer->display-center direction: the caller's off-axis
	// Kooima correction already accounts for the eye offset, and using the
	// center direction here would double-count the downward tilt (seen as the
	// top half of the window never selecting tiles).
	glm::dvec3 dXr(0, 0, -1);
	glm::dvec3 dEcef = glm::normalize(enuRot * (glm::transpose(rot) * dXr));
	glm::dvec3 uEcef = glm::normalize(enuRot * (glm::transpose(rot) * glm::dvec3(0, 1, 0)));

	double zdp = std::max(viewerPosXr.z, 0.1);
	GeoCamera c;
	c.pos = anchorEcef - dEcef * (zdp / s);
	c.dir = dEcef;
	c.up = uEcef;
	return c;
}

void
GeoNav::frameBookmark(int index)
{
	size_t n = 0;
	const Bookmark *bm = bookmarks(&n);
	bookmarkIndex = ((index % (int)n) + (int)n) % (int)n;
	cam = cameraForBookmark(bm[bookmarkIndex]);
	targetDist = bm[bookmarkIndex].distM;
	orbitAcquired = false;
	dioramaYaw = 0.0;
	dioramaTilt = 0.0;
}

void
GeoNav::cycleBookmark()
{
	frameBookmark(bookmarkIndex + 1);
}

void
GeoNav::look(double dYawRad, double dPitchRad)
{
	if (orbitAcquired) {
		// Diorama: drag spins + tilts the tabletop itself (display-centric).
		dioramaYaw += dYawRad;
		dioramaTilt = std::clamp(dioramaTilt - dPitchRad, 0.0, glm::radians(85.0));
		return;
	}
	// Free-look: yaw about local up, pitch about camera right.
	glm::dvec3 upN = glm::normalize(wgs84().geodeticSurfaceNormal(cam.pos));
	glm::dmat4 ry = glm::rotate(glm::dmat4(1.0), -dYawRad, upN);
	glm::dvec3 d = glm::normalize(glm::dvec3(ry * glm::dvec4(cam.dir, 0.0)));
	glm::dvec3 right = glm::normalize(glm::cross(d, upN));
	glm::dmat4 rp = glm::rotate(glm::dmat4(1.0), -dPitchRad, right);
	glm::dvec3 d2 = glm::normalize(glm::dvec3(rp * glm::dvec4(d, 0.0)));
	// Avoid gimbal flip at the poles of the look sphere.
	if (std::abs(glm::dot(d2, upN)) < 0.98) {
		d = d2;
	}
	cam.dir = d;
	cam.up = upN;
}

void
GeoNav::pan(double dxNorm, double dyNorm)
{
	if (orbitAcquired) {
		// Slide the table in the DISPLAYED frame: screen-right and
		// screen-forward (viewer axis projected onto the table plane),
		// mapped through the diorama rotation — W always pushes the city
		// away from you regardless of spin. Tilt is ignored for pan (the
		// table plane is the reference).
		glm::dmat3 rot = dioramaRotation(dioramaYaw, 0.0);
		glm::dmat3 enuRot = glm::dmat3(enuFrame(orbitCenter));
		glm::dvec3 rightEnu = glm::transpose(rot) * glm::dvec3(1, 0, 0);
		glm::dvec3 fwdEnu = glm::transpose(rot) * glm::dvec3(0, 0, -1);
		rightEnu.z = 0.0;
		fwdEnu.z = 0.0;
		glm::dvec3 delta = enuRot * (glm::normalize(rightEnu) * dxNorm +
		                             glm::normalize(fwdEnu) * dyNorm) *
		                   targetDist;
		orbitCenter += delta;
		cam.pos += delta;
		return;
	}
	glm::dvec3 upN = glm::normalize(wgs84().geodeticSurfaceNormal(cam.pos));
	glm::dvec3 right = glm::normalize(glm::cross(cam.dir, upN));
	// Ground-plane forward (camera dir projected onto the tangent plane).
	glm::dvec3 fwd = cam.dir - upN * glm::dot(cam.dir, upN);
	double fl = glm::length(fwd);
	fwd = fl > 1e-9 ? fwd / fl : glm::normalize(glm::cross(upN, right));
	glm::dvec3 delta = (right * dxNorm + fwd * dyNorm) * targetDist;
	cam.pos += delta;
}

void
GeoNav::dolly(double steps)
{
	// Exponential zoom: each step covers 10% of the remaining distance.
	double k = std::pow(0.9, steps);
	if (orbitAcquired) {
		// Display-centric diorama: wheel scales the tabletop (the geo camera
		// doesn't drive the rig in this mode). Scroll up = bigger city.
		dioramaScale = std::clamp(dioramaScale / k, 1.0 / 50000.0, 1.0 / 200.0);
		// Equivalent viewing distance at this scale (nominal 0.6 m viewer):
		// keeps SSE detail and pan speed proportional to the visible footprint.
		targetDist = 0.6 / dioramaScale;
		return;
	}
	cam.pos += cam.dir * (targetDist * (1.0 - k));
	targetDist = std::max(targetDist * k, 20.0);
}

void
GeoNav::elevate(double dyNorm)
{
	glm::dvec3 upN = glm::normalize(wgs84().geodeticSurfaceNormal(cam.pos));
	cam.pos += upN * (dyNorm * targetDist);
}

void
GeoNav::acquireOrbit(const glm::dvec3 &centerEcef, double zdpXr)
{
	const bool reAcquire = orbitAcquired;
	orbitAcquired = true;
	orbitCenter = centerEcef;
	targetDist = glm::length(cam.pos - centerEcef);
	cam.dir = glm::normalize(centerEcef - cam.pos);
	cam.up = glm::normalize(wgs84().geodeticSurfaceNormal(centerEcef));

	// Re-centering within the diorama (double-click on another spot): keep
	// the user's zoom / spin / tilt — only the center moves.
	if (reAcquire) {
		return;
	}

	// First acquire: seed the diorama from the camera view so the transition
	// preserves the apparent framing (no zoom / orientation jump):
	//  - yaw  = the camera's compass azimuth at the picked point
	//  - tilt = the camera's elevation above the local horizon
	//  - scale = same apparent magnification (zdp / distance-to-center)
	glm::dmat3 enuRot = glm::dmat3(enuFrame(centerEcef));
	glm::dvec3 dEnu = glm::transpose(enuRot) * cam.dir;
	dioramaYaw = std::atan2(dEnu.x, dEnu.y); // azimuth: 0 = north, +east
	dioramaTilt = std::clamp(std::asin(std::clamp(-dEnu.z, -1.0, 1.0)), 0.0,
	                         glm::radians(85.0));
	dioramaScale = std::clamp(stereoScaleForDistance(targetDist, zdpXr),
	                          1.0 / 50000.0, 1.0 / 200.0);
}

void
GeoNav::releaseToFly(double zdpXr)
{
	if (!orbitAcquired) {
		return;
	}
	// Continuity: become the free camera that sees what the diorama showed —
	// the viewer axis mapped through the diorama rotation, standing back at
	// the zoom-equivalent distance.
	GeoCamera c = dioramaSelectionCamera(orbitCenter, dioramaScale, dioramaYaw, dioramaTilt,
	                                     glm::dvec3(0.0, 0.0, zdpXr));
	cam.pos = c.pos;
	cam.dir = c.dir;
	cam.up = glm::normalize(wgs84().geodeticSurfaceNormal(c.pos)); // nav up
	targetDist = std::max(zdpXr, 0.1) / dioramaScale;
	orbitAcquired = false;
}

void
GeoNav::releaseOrbit()
{
	frameBookmark(bookmarkIndex);
}

} // namespace geo
