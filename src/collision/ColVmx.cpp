// ColVmx.cpp -- from res-PS3 (patch 62): the collision broadphase with AltiVec.
//
// ProcessColModels tests model A's bounding sphere against every triangle of
// model B (a building: hundreds or thousands) before the exact tests. Here the
// triangles' bounding boxes (ColModel.cpp, kept with the planes) are compared
// with the sphere's box four at a time; only the triangles whose box touches
// it go on to the exact test (CCollision::TestSphereTriangle), so the result
// is the same: a triangle whose box doesn't touch the sphere's can't touch
// the sphere. Built with -maltivec (Makefile); the rest of the game isn't.

#include <stdint.h>

#if defined(__PS3__) && defined(__ALTIVEC__)
#include <altivec.h>
// a float in the four lanes (no vec_splats on every toolchain)
static inline vector float
splat4(float f)
{
	float v[4] __attribute__((aligned(16))) = { f, f, f, f };
	return vec_ld(0, v);
}
#endif

// boxes: min x[n4], min y[n4], min z[n4], max x[n4], max y[n4], max z[n4],
// 16-byte aligned, n4 = numTriangles rounded up to 4 (the padding empty).
// out: the indices of the triangles whose box touches the sphere's box
// (grown by eps). Returns how many.
int
PS3_ColBoxCandidates(const float *boxes, int numTriangles, float cx, float cy, float cz, float r, float eps, int *out)
{
	int n4 = (numTriangles + 3) & ~3;
	float lo[3] = { cx - r - eps, cy - r - eps, cz - r - eps };
	float hi[3] = { cx + r + eps, cy + r + eps, cz + r + eps };
	int n = 0;
#if defined(__PS3__) && defined(__ALTIVEC__)
	const float *mnx = boxes, *mny = boxes + n4, *mnz = boxes + 2 * n4;
	const float *mxx = boxes + 3 * n4, *mxy = boxes + 4 * n4, *mxz = boxes + 5 * n4;
	vector float vlox = splat4(lo[0]), vloy = splat4(lo[1]), vloz = splat4(lo[2]);
	vector float vhix = splat4(hi[0]), vhiy = splat4(hi[1]), vhiz = splat4(hi[2]);
	const vector unsigned int zero = vec_splat_u32(0);
	for (int i = 0; i < n4; i += 4) {
		// touches: max >= lo and min <= hi on the three axes
		vector bool int m = vec_and(vec_cmpge(vec_ld(0, mxx + i), vlox), vec_cmpge(vhix, vec_ld(0, mnx + i)));
		m = vec_and(m, vec_and(vec_cmpge(vec_ld(0, mxy + i), vloy), vec_cmpge(vhiy, vec_ld(0, mny + i))));
		m = vec_and(m, vec_and(vec_cmpge(vec_ld(0, mxz + i), vloz), vec_cmpge(vhiz, vec_ld(0, mnz + i))));
		if (!vec_any_ne((vector unsigned int)m, zero))
			continue;
		unsigned int lanes[4] __attribute__((aligned(16)));
		vec_st((vector unsigned int)m, 0, lanes);
		for (int k = 0; k < 4; k++)
			if (lanes[k] && i + k < numTriangles)
				out[n++] = i + k;
	}
#else
	for (int i = 0; i < numTriangles; i++)
		if (boxes[3 * n4 + i] >= lo[0] && boxes[i] <= hi[0] &&
		    boxes[4 * n4 + i] >= lo[1] && boxes[n4 + i] <= hi[1] &&
		    boxes[5 * n4 + i] >= lo[2] && boxes[2 * n4 + i] <= hi[2])
			out[n++] = i;
#endif
	return n;
}
