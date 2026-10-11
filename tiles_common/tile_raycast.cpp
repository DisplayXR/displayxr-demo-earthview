// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// See tile_raycast.h.

#include "tile_raycast.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr uint32_t kLeafTris = 4;
} // namespace

void
TileBvh::buildInto(uint32_t slot, uint32_t first, uint32_t count, std::vector<uint32_t> &order,
                   const std::vector<float> &c)
{
	float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
	float cmin[3] = {1e30f, 1e30f, 1e30f}, cmax[3] = {-1e30f, -1e30f, -1e30f};
	for (uint32_t k = first; k < first + count; ++k) {
		const uint32_t ti = order[k];
		for (int v = 0; v < 3; ++v) {
			const float *p = &pos_[(size_t)tris_[ti * 3 + v] * 3];
			for (int a = 0; a < 3; ++a) {
				bmin[a] = std::min(bmin[a], p[a]);
				bmax[a] = std::max(bmax[a], p[a]);
			}
		}
		for (int a = 0; a < 3; ++a) {
			cmin[a] = std::min(cmin[a], c[ti * 3 + a]);
			cmax[a] = std::max(cmax[a], c[ti * 3 + a]);
		}
	}
	Node n{};
	std::memcpy(n.bmin, bmin, sizeof(bmin));
	std::memcpy(n.bmax, bmax, sizeof(bmax));
	int axis = 0;
	for (int a = 1; a < 3; ++a)
		if (cmax[a] - cmin[a] > cmax[axis] - cmin[axis]) axis = a;
	if (count <= kLeafTris || !(cmax[axis] - cmin[axis] > 0.0f)) {
		n.first = first; // a range of `order`; build() applies it to tris_
		n.count = count;
		nodes_[slot] = n;
		return;
	}
	const uint32_t half = count / 2;
	std::nth_element(order.begin() + first, order.begin() + first + half, order.begin() + first + count,
	                 [&](uint32_t x, uint32_t y) { return c[x * 3 + axis] < c[y * 3 + axis]; });
	const uint32_t left = (uint32_t)nodes_.size();
	nodes_.push_back(Node{});
	nodes_.push_back(Node{});
	n.first = left;
	n.count = 0;
	nodes_[slot] = n;
	buildInto(left, first, half, order, c);
	buildInto(left + 1, first + half, count - half, order, c);
}

void
TileBvh::build(const float *positionsXyz,
               uint32_t vertexCount,
               const uint32_t *indices,
               uint32_t indexCount,
               const float *frame)
{
	static const float kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	std::memcpy(frame_, frame ? frame : kIdentity, sizeof(frame_));
	pos_.resize((size_t)vertexCount * 3);
	for (uint32_t i = 0; i < vertexCount; ++i) {
		const float *p = positionsXyz + (size_t)i * 3;
		for (int r = 0; r < 3; ++r) {
			pos_[(size_t)i * 3 + r] = frame_[r * 3] * p[0] + frame_[r * 3 + 1] * p[1] + frame_[r * 3 + 2] * p[2];
		}
	}
	tris_.clear();
	nodes_.clear();
	tris_.reserve(indexCount);
	for (uint32_t k = 0; k + 2 < indexCount; k += 3) {
		const uint32_t i0 = indices[k], i1 = indices[k + 1], i2 = indices[k + 2];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
		tris_.push_back(i0);
		tris_.push_back(i1);
		tris_.push_back(i2);
	}
	const uint32_t triCount = (uint32_t)(tris_.size() / 3);
	if (triCount == 0) {
		pos_.clear();
		return;
	}
	std::vector<float> cent((size_t)triCount * 3);
	for (uint32_t t = 0; t < triCount; ++t) {
		const float *a = &pos_[(size_t)tris_[t * 3] * 3];
		const float *b = &pos_[(size_t)tris_[t * 3 + 1] * 3];
		const float *c = &pos_[(size_t)tris_[t * 3 + 2] * 3];
		for (int x = 0; x < 3; ++x) cent[(size_t)t * 3 + x] = (a[x] + b[x] + c[x]) * (1.0f / 3.0f);
	}
	std::vector<uint32_t> order(triCount);
	for (uint32_t t = 0; t < triCount; ++t) order[t] = t;
	nodes_.reserve((size_t)triCount / kLeafTris * 2 + 2);
	nodes_.push_back(Node{});
	buildInto(0, 0, triCount, order, cent);
	// Leaves index ranges of `order`; make them ranges of tris_ directly.
	std::vector<uint32_t> sorted(tris_.size());
	for (uint32_t k = 0; k < triCount; ++k) {
		const uint32_t s = order[k];
		sorted[k * 3] = tris_[s * 3];
		sorted[k * 3 + 1] = tris_[s * 3 + 1];
		sorted[k * 3 + 2] = tris_[s * 3 + 2];
	}
	tris_.swap(sorted);
	nodes_.shrink_to_fit();
}

bool
TileBvh::raycast(const float o[3], const float d[3], const float invD[3], float &tInOut) const
{
	if (nodes_.empty()) return false;
	float best = tInOut;
	bool hit = false;
	uint32_t stack[64];
	uint32_t sp = 0;
	stack[sp++] = 0;
	while (sp > 0) {
		const Node &n = nodes_[stack[--sp]];
		if (tileRayAabb(o, invD, n.bmin, n.bmax, best) < 0.0f) continue;
		if (n.count > 0) {
			for (uint32_t k = n.first; k < n.first + n.count; ++k) {
				const float *v0 = &pos_[(size_t)tris_[k * 3] * 3];
				const float *v1 = &pos_[(size_t)tris_[k * 3 + 1] * 3];
				const float *v2 = &pos_[(size_t)tris_[k * 3 + 2] * 3];
				const float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
				const float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
				// Moller-Trumbore, two-sided.
				const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
				                    d[0] * e2[1] - d[1] * e2[0]};
				const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
				if (std::fabs(det) < 1e-30f) continue;
				const float inv = 1.0f / det;
				const float s[3] = {o[0] - v0[0], o[1] - v0[1], o[2] - v0[2]};
				const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
				if (u < 0.0f || u > 1.0f) continue;
				const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
				                    s[0] * e1[1] - s[1] * e1[0]};
				const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
				if (v < 0.0f || u + v > 1.0f) continue;
				const float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
				if (tt >= 0.0f && tt < best) {
					best = tt;
					hit = true;
				}
			}
		} else if (sp + 2 <= 64) {
			// Nearer child last (popped first): fewer triangles once `best` shrinks.
			const Node &l = nodes_[n.first], &r = nodes_[n.first + 1];
			const float tl = tileRayAabb(o, invD, l.bmin, l.bmax, best);
			const float tr = tileRayAabb(o, invD, r.bmin, r.bmax, best);
			if (tl >= 0.0f && tr >= 0.0f) {
				if (tl <= tr) {
					stack[sp++] = n.first + 1;
					stack[sp++] = n.first;
				} else {
					stack[sp++] = n.first;
					stack[sp++] = n.first + 1;
				}
			} else if (tl >= 0.0f) {
				stack[sp++] = n.first;
			} else if (tr >= 0.0f) {
				stack[sp++] = n.first + 1;
			}
		}
	}
	if (hit) tInOut = best;
	return hit;
}
