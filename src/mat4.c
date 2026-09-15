/*
spnav-onshape-bridge - bridges spacenavd 3D-mouse events to Onshape's
browser 3Dconnexion API on Linux.
Copyright (C) 2026 Sebastian Polster <polsterseb@protonmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <math.h>
#include <string.h>
#include "mat4.h"

void mat4_identity(float m[16])
{
	memset(m, 0, 16 * sizeof *m);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_copy(float out[16], const float in[16])
{
	memcpy(out, in, 16 * sizeof *out);
}

void mat4_mul(float out[16], const float a[16], const float b[16])
{
	float res[16];
	int i, j, k;

	for(i=0; i<4; i++) {
		for(j=0; j<4; j++) {
			float sum = 0.0f;
			for(k=0; k<4; k++) {
				sum += a[i * 4 + k] * b[k * 4 + j];
			}
			res[i * 4 + j] = sum;
		}
	}
	memcpy(out, res, sizeof res);
}

void mat3_mul(float out[9], const float a[9], const float b[9])
{
	float res[9];
	int i, j, k;

	for(i=0; i<3; i++) {
		for(j=0; j<3; j++) {
			float sum = 0.0f;
			for(k=0; k<3; k++) {
				sum += a[i * 3 + k] * b[k * 3 + j];
			}
			res[i * 3 + j] = sum;
		}
	}
	memcpy(out, res, sizeof res);
}

void mat3_transpose(float out[9], const float in[9])
{
	float res[9];
	int i, j;

	for(i=0; i<3; i++) {
		for(j=0; j<3; j++) {
			res[i * 3 + j] = in[j * 3 + i];
		}
	}
	memcpy(out, res, sizeof res);
}

static float vdot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vnormalize(float *v)
{
	float len = sqrtf(vdot(v, v));
	if(len > 1e-8f) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

/* Classic Gram-Schmidt orthonormalization of the rows of a 3x3 matrix.
 * Not the nearest orthogonal matrix (that would need SVD/polar decomposition),
 * but sufficient to correct the small per-frame numerical drift this is used
 * for, without a linear algebra library.
 */
static void gram_schmidt3(float r[9])
{
	float *r0 = r, *r1 = r + 3, *r2 = r + 6;
	float d;

	vnormalize(r0);

	d = vdot(r1, r0);
	r1[0] -= d * r0[0]; r1[1] -= d * r0[1]; r1[2] -= d * r0[2];
	vnormalize(r1);

	d = vdot(r2, r0);
	r2[0] -= d * r0[0]; r2[1] -= d * r0[1]; r2[2] -= d * r0[2];
	d = vdot(r2, r1);
	r2[0] -= d * r1[0]; r2[1] -= d * r1[1]; r2[2] -= d * r1[2];
	vnormalize(r2);
}

void mat4_extract_orthonormal_rotation(float r[9], const float affine[16])
{
	int i, j;

	/* transpose of the top-left 3x3 block: r[i][j] = affine[j][i] */
	for(i=0; i<3; i++) {
		for(j=0; j<3; j++) {
			r[i * 3 + j] = affine[j * 4 + i];
		}
	}
	gram_schmidt3(r);
}

void mat3_from_euler_xyz_deg(float r[9], float ax, float ay, float az)
{
	float rx[9], ry[9], rz[9], tmp[9];
	float a = ax * (float)M_PI / 180.0f;
	float b = ay * (float)M_PI / 180.0f;
	float c = az * (float)M_PI / 180.0f;
	float ca = cosf(a), sa = sinf(a);
	float cb = cosf(b), sb = sinf(b);
	float cc = cosf(c), sc = sinf(c);

	/* intrinsic xyz (scipy "xyz"): R = Rx(a) @ Ry(b) @ Rz(c) */
	rx[0]=1;    rx[1]=0;    rx[2]=0;
	rx[3]=0;    rx[4]=ca;   rx[5]=-sa;
	rx[6]=0;    rx[7]=sa;   rx[8]=ca;

	ry[0]=cb;   ry[1]=0;    ry[2]=sb;
	ry[3]=0;    ry[4]=1;    ry[5]=0;
	ry[6]=-sb;  ry[7]=0;    ry[8]=cb;

	rz[0]=cc;   rz[1]=-sc;  rz[2]=0;
	rz[3]=sc;   rz[4]=cc;   rz[5]=0;
	rz[6]=0;    rz[7]=0;    rz[8]=1;

	mat3_mul(tmp, rx, ry);
	mat3_mul(r, tmp, rz);
}

void mat4_from_rot3(float out[16], const float r[9])
{
	int i, j;

	mat4_identity(out);
	for(i=0; i<3; i++) {
		for(j=0; j<3; j++) {
			out[i * 4 + j] = r[i * 3 + j];
		}
	}
}

void mat4_from_translation_row(float out[16], float tx, float ty, float tz)
{
	mat4_identity(out);
	out[3 * 4 + 0] = tx;
	out[3 * 4 + 1] = ty;
	out[3 * 4 + 2] = tz;
}
