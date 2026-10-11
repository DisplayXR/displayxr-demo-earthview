// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// tile_raycast — a CPU BVH over ONE streamed tile's triangles, in the tile's
// own (tile-local, RTC float) space: the depth source of the depth-aware
// cursor (XR_DXR_cursor_depth, runtime ADR-046; docs/depth-cursor.md).
//
// Built ONCE per tile, on cesium's LOAD (worker) thread inside
// TileRenderer::prepareInLoadThread, from the same float vertices the GPU
// gets — so a frame never pays for a build and the render thread never
// touches a triangle it does not need. Queried on the render thread through
// TileRenderer::raycast, which only ever visits the tiles drawn last frame.
//
// Shape (adapted from displayxr-demo-modelviewer's model_raycast, one cluster
// per tile): median split on the widest centroid axis, <= 4 triangles per
// leaf, indexed triangles (positions + a reordered index triplet — ~18 B per
// triangle, vs 36 B pre-differenced, because a city keeps millions resident),
// Moller-Trumbore two-sided (the cursor wants the nearest surface whatever its
// winding).

#pragma once

#include <cstdint>
#include <vector>

class TileBvh
{
public:
	//! Build from tile-local positions (xyz float triples) + a triangle index
	//! list (3 per triangle; out-of-range triangles are dropped). Replaces any
	//! previous content.
	//!
	//! `frame` (row-major 3x3, null = identity) is a linear map applied to every
	//! position first: the BVH lives in "BVH space" = frame * tile-local. The
	//! caller passes the tile's local EAST/NORTH/UP axes: streamed tile meshes
	//! are ECEF-axis-aligned, so at 48 deg latitude a flat street block is a
	//! 45-deg-tilted slab whose axis-aligned boxes are as tall as they are wide
	//! — and a ray grazing toward the horizon enters every one of them. In ENU
	//! the same boxes are flat. Any invertible linear map preserves the ray t.
	void
	build(const float *positionsXyz,
	      uint32_t vertexCount,
	      const uint32_t *indices,
	      uint32_t indexCount,
	      const float *frame = nullptr);

	//! The `frame` the BVH was built with (row-major 3x3).
	const float *
	frame() const
	{
		return frame_;
	}

	//! Nearest hit along origin + t * dir, 0 <= t < tMax, in BVH space (see
	//! build). dir need not be unit (t is in its units; a linear/affine map
	//! from the world preserves t). invDir = 1/dir per axis (+-inf allowed).
	//! False on a miss.
	bool
	raycast(const float origin[3], const float dir[3], const float invDir[3], float &tInOut) const;

	//! Root bounds (BVH space). Valid only when !empty().
	const float *
	boundsMin() const
	{
		return nodes_.empty() ? nullptr : nodes_[0].bmin;
	}
	const float *
	boundsMax() const
	{
		return nodes_.empty() ? nullptr : nodes_[0].bmax;
	}

	bool
	empty() const
	{
		return nodes_.empty();
	}
	uint32_t
	triangleCount() const
	{
		return (uint32_t)(tris_.size() / 3);
	}
	uint64_t
	bytes() const
	{
		return pos_.size() * sizeof(float) + tris_.size() * sizeof(uint32_t) + nodes_.size() * sizeof(Node);
	}

private:
	struct Node // 32 bytes
	{
		float bmin[3];
		uint32_t first; // leaf: first triangle; inner: left child (right = left + 1)
		float bmax[3];
		uint32_t count; // > 0 = leaf
	};

	void
	buildInto(uint32_t slot, uint32_t first, uint32_t count, std::vector<uint32_t> &order, const std::vector<float> &c);

	float frame_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	std::vector<float> pos_;     // xyz per vertex (BVH space = frame_ * tile-local)
	std::vector<uint32_t> tris_; // 3 vertex indices per triangle, in leaf order
	std::vector<Node> nodes_;
};

//! Slab test: entry t (clamped at 0), or a negative value on a miss / beyond tMax.
inline float
tileRayAabb(const float o[3], const float invD[3], const float bmin[3], const float bmax[3], float tMax)
{
	float t0 = 0.0f, t1 = tMax;
	for (int a = 0; a < 3; ++a) {
		float ta = (bmin[a] - o[a]) * invD[a];
		float tb = (bmax[a] - o[a]) * invD[a];
		if (ta > tb) {
			const float s = ta;
			ta = tb;
			tb = s;
		}
		// NaN (0 * inf on a slab boundary) must not shrink the interval.
		if (ta > t0) t0 = ta;
		if (tb < t1) t1 = tb;
		if (t0 > t1) return -1.0f;
	}
	return t0;
}
