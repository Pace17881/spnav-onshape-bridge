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
