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
#ifndef MAT4_H_
#define MAT4_H_

/* Column-major-agnostic: we just treat everything as a flat 16-float array
 * in the same row-major layout Onshape's view.affine uses (row vectors,
 * translation in the last row - m[12..14]), matching controller.py exactly.
 */

void mat4_identity(float m[16]);
void mat4_mul(float out[16], const float a[16], const float b[16]);
void mat4_copy(float out[16], const float in[16]);

/* Extract the 3x3 upper-left block, transpose it (matches controller.py's
 * R_cam = curr_affine[:3,:3].T) and re-orthonormalize it via Gram-Schmidt to
 * correct for drift, replacing the numpy SVD approach without needing LAPACK.
 */
void mat4_extract_orthonormal_rotation(float r[9], const float affine[16]);

/* Build a rotation-delta matrix from intrinsic xyz Euler angles in degrees,
 * matching scipy's Rotation.from_euler("xyz", angles, degrees=True).
 */
void mat3_from_euler_xyz_deg(float r[9], float ax, float ay, float az);

void mat3_mul(float out[9], const float a[9], const float b[9]);
void mat3_transpose(float out[9], const float in[9]);

/* Build a 4x4 matrix with the given 3x3 rotation in the upper-left block
 * and identity elsewhere (matches np.eye(4); result[:3,:3] = R). */
void mat4_from_rot3(float out[16], const float r[9]);

/* Build a 4x4 pure-translation matrix following the row-vector convention
 * used here: translation goes in row 3 (m[12..14]), matching
 * `trans_delta[3, :3] = ...` in controller.py. */
void mat4_from_translation_row(float out[16], float tx, float ty, float tz);

#endif
