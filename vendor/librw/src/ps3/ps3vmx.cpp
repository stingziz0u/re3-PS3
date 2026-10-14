// ps3vmx.cpp -- AltiVec (VMX) on the PPU. Built with -maltivec (the rest
// of the game isn't). The mip levels made on the CPU (vmxDownsample, patch
// 41: the same bytes as ps3raster.cpp's scalar filters), and the hardware
// probe's test (USRDIR/probe.txt): a 2x2 mip level of a 512x512 RGBA8
// picture, compared and timed against the scalar code.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <altivec.h>
#include <sys/systime.h>

typedef uint8_t uint8;
typedef uint32_t uint32;

static void
downScalar(uint8 *dst, const uint8 *src, int sw, int sh)
{
	int dw = sw/2, dh = sh/2;
	for(int y = 0; y < dh; y++){
		const uint8 *r0 = src + (2*y)*sw*4, *r1 = r0 + sw*4;
		uint8 *d = dst + y*dw*4;
		for(int x = 0; x < dw; x++)
			for(int k = 0; k < 4; k++)
				d[x*4+k] = (r0[x*8+k] + r0[x*8+4+k] + r1[x*8+k] + r1[x*8+4+k] + 2) >> 2;
	}
}

// 8 source pixels of two rows -> 4 destination pixels a step (sw a
// multiple of 8, buffers 16-byte aligned)
static void
downVMX(uint8 *dst, const uint8 *src, int sw, int sh)
{
	const vector unsigned char evenPx = { 0,1,2,3, 8,9,10,11, 16,17,18,19, 24,25,26,27 };
	const vector unsigned char oddPx  = { 4,5,6,7, 12,13,14,15, 20,21,22,23, 28,29,30,31 };
	const vector unsigned char zero = vec_splat_u8(0);
	const vector unsigned short two = vec_splat_u16(2);
	int dw = sw/2, dh = sh/2;
	for(int y = 0; y < dh; y++){
		const uint8 *r0 = src + (2*y)*sw*4, *r1 = r0 + sw*4;
		uint8 *d = dst + y*dw*4;
		for(int x = 0; x < sw; x += 8){
			vector unsigned char a0 = vec_ld(0, r0 + x*4), a1 = vec_ld(16, r0 + x*4);
			vector unsigned char b0 = vec_ld(0, r1 + x*4), b1 = vec_ld(16, r1 + x*4);
			vector unsigned char ea = vec_perm(a0, a1, evenPx), oa = vec_perm(a0, a1, oddPx);
			vector unsigned char eb = vec_perm(b0, b1, evenPx), ob = vec_perm(b0, b1, oddPx);
			// to 16 bit lanes (big-endian: merge the zero byte in front)
			vector unsigned short hi = vec_add(vec_add((vector unsigned short)vec_mergeh(zero, ea), (vector unsigned short)vec_mergeh(zero, oa)),
			                                   vec_add((vector unsigned short)vec_mergeh(zero, eb), (vector unsigned short)vec_mergeh(zero, ob)));
			vector unsigned short lo = vec_add(vec_add((vector unsigned short)vec_mergel(zero, ea), (vector unsigned short)vec_mergel(zero, oa)),
			                                   vec_add((vector unsigned short)vec_mergel(zero, eb), (vector unsigned short)vec_mergel(zero, ob)));
			hi = vec_sr(vec_add(hi, two), two);
			lo = vec_sr(vec_add(lo, two), two);
			vec_st(vec_packsu(hi, lo), 0, d + (x/2)*4);
		}
	}
}

// downsampleCoverage of ps3raster.cpp for one destination texel (not all
// four source texels opaque)
static void
coverageTexel(uint8 *d, const uint8 *p0, const uint8 *p1, const uint8 *p2, const uint8 *p3)
{
	const uint8 *p[4] = { p0, p1, p2, p3 };
	int asum = 0, amax = 0;
	for(int i = 0; i < 4; i++){
		asum += p[i][3];
		if(p[i][3] > amax) amax = p[i][3];
	}
	for(int k = 0; k < 3; k++){
		if(asum > 0){
			int c = 0;
			for(int i = 0; i < 4; i++)
				c += p[i][k] * p[i][3];
			d[k] = (c + asum/2) / asum;
		}else
			d[k] = (p[0][k] + p[1][k] + p[2][k] + p[3][k] + 2) >> 2;
	}
	d[3] = ((asum + 2)/4 + amax + 1) / 2;
}

// A 2x2 mip level of RGBA8 bytes: the rounded average ((a+b+c+d+2)>>2), or
// with coverage, where the four texels aren't all opaque, downsampleCoverage's
// filter (alpha weighted colour). 0: not done (the level isn't exactly half
// of one at least 8 wide, or a buffer isn't 16 byte aligned).
extern "C" int
vmxDownsample(uint8 *dst, int dw, int dh, const uint8 *src, int sw, int sh, int coverage)
{
	if(sw != 2*dw || sh != 2*dh || (sw & 7) || dh < 1 ||
	   ((uintptr_t)dst & 15) || ((uintptr_t)src & 15))
		return 0;
	const vector unsigned char evenPx = { 0,1,2,3, 8,9,10,11, 16,17,18,19, 24,25,26,27 };
	const vector unsigned char oddPx  = { 4,5,6,7, 12,13,14,15, 20,21,22,23, 28,29,30,31 };
	// 0xFF but on the alpha bytes: an AND of texels ORed with it is all
	// ones only if every alpha was 255
	const vector unsigned char rgbOnes = { 255,255,255,0, 255,255,255,0, 255,255,255,0, 255,255,255,0 };
	const vector unsigned char ones = vec_splat_u8(-1);
	const vector unsigned char zero = vec_splat_u8(0);
	const vector unsigned short two = vec_splat_u16(2);
	for(int y = 0; y < dh; y++){
		const uint8 *r0 = src + (2*y)*sw*4, *r1 = r0 + sw*4;
		uint8 *d = dst + y*dw*4;
		for(int x = 0; x < sw; x += 8){
			vector unsigned char a0 = vec_ld(0, r0 + x*4), a1 = vec_ld(16, r0 + x*4);
			vector unsigned char b0 = vec_ld(0, r1 + x*4), b1 = vec_ld(16, r1 + x*4);
			vector unsigned char ea = vec_perm(a0, a1, evenPx), oa = vec_perm(a0, a1, oddPx);
			vector unsigned char eb = vec_perm(b0, b1, evenPx), ob = vec_perm(b0, b1, oddPx);
			// to 16 bit lanes (big-endian: the zero byte in front)
			vector unsigned short hi = vec_add(vec_add((vector unsigned short)vec_mergeh(zero, ea), (vector unsigned short)vec_mergeh(zero, oa)),
			                                   vec_add((vector unsigned short)vec_mergeh(zero, eb), (vector unsigned short)vec_mergeh(zero, ob)));
			vector unsigned short lo = vec_add(vec_add((vector unsigned short)vec_mergel(zero, ea), (vector unsigned short)vec_mergel(zero, oa)),
			                                   vec_add((vector unsigned short)vec_mergel(zero, eb), (vector unsigned short)vec_mergel(zero, ob)));
			hi = vec_sr(vec_add(hi, two), two);
			lo = vec_sr(vec_add(lo, two), two);
			uint8 *o = d + (x/2)*4;
			vec_st(vec_packsu(hi, lo), 0, o);
			if(coverage){
				vector unsigned char m = vec_or(vec_and(vec_and(a0, a1), vec_and(b0, b1)), rgbOnes);
				if(!vec_all_eq(m, ones)){
					// some texel not opaque: those groups the scalar way
					const uint8 *s0 = r0 + x*4, *s1 = r1 + x*4;
					for(int k = 0; k < 4; k++){
						const uint8 *p0 = s0 + k*8, *p1 = p0 + 4, *p2 = s1 + k*8, *p3 = p2 + 4;
						if((p0[3] & p1[3] & p2[3] & p3[3]) != 255)
							coverageTexel(o + k*4, p0, p1, p2, p3);
					}
				}
			}
		}
	}
	return 1;
}

extern "C" int
ps3VmxProbe(char *out, int size)
{
	const int W = 512, H = 512, N = 20;
	uint8 *src = (uint8*)memalign(128, W*H*4);
	uint8 *a = (uint8*)memalign(128, W*H);
	uint8 *b = (uint8*)memalign(128, W*H);
	if(src == NULL || a == NULL || b == NULL){
		snprintf(out, size, "no memory for the test");
		free(src); free(a); free(b);
		return -1;
	}
	uint32 seed = 777;
	for(int i = 0; i < W*H*4; i++){ seed = seed*1103515245 + 12345; src[i] = seed >> 24; }
	uint64_t t0 = sysGetSystemTime();
	for(int i = 0; i < N; i++)
		downScalar(a, src, W, H);
	uint64_t t1 = sysGetSystemTime();
	for(int i = 0; i < N; i++)
		downVMX(b, src, W, H);
	uint64_t t2 = sysGetSystemTime();
	int diff = 0;
	for(int i = 0; i < (W/2)*(H/2)*4; i++)
		diff += a[i] != b[i];
	double ms0 = (t1 - t0)/1000.0/N, ms1 = (t2 - t1)/1000.0/N;
	snprintf(out, size, "512x512 mip level: scalar %.2f ms, AltiVec %.2f ms (%.1fx); %d of %d bytes differ (0: the same result)",
	         ms0, ms1, ms1 > 0.0 ? ms0/ms1 : 0.0, diff, (W/2)*(H/2)*4);
	free(src); free(a); free(b);
	return diff == 0 ? 0 : -1;
}
