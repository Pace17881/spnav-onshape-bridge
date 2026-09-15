#include <stdio.h>
#include <assert.h>
#include <math.h>
#include "mat4.h"

int main(void)
{
	float curr_affine[16] = {0.9271885408792452f,  -0.3671419119088283f, 0.07435204221907192f, 0,
							 0.08448485590856766f, 0.3983266795284326f,	 0.9133444944258232f,  0,
							 -0.3649434460081796f, -0.8405609275333846f, 0.4003418643108554f,  0,
							 0.08627579426964002f, -0.2486249618828344f, 0.1137656502994606f,  1};
	float pitch = 12.0f, yaw = -7.0f, roll = 3.0f;
	float x = 40.0f, y = -15.0f, z = 22.0f;
	float model_extents[6] = {-10, -5, -8, 10, 5, 8};

	float r_cam[9], r_cam_t[9], r_delta[9], r_world[9];
	float rot_delta[16], trans_delta[16];
	float pivot[3], pivot_pos[16], pivot_neg[16];
	float tmp16a[16], tmp16b[16], new_affine[16];
	int i;

	mat4_extract_orthonormal_rotation(r_cam, curr_affine);
	printf("R_cam ortho:\n");
	for(i = 0; i < 9; i++)
		printf("%.7f%s", r_cam[i], (i % 3 == 2) ? "\n" : " ");

	mat3_from_euler_xyz_deg(r_delta, pitch * 0.02f, yaw * 0.02f, -roll * 0.02f);
	printf("R_delta_cam:\n");
	for(i = 0; i < 9; i++)
		printf("%.7f%s", r_delta[i], (i % 3 == 2) ? "\n" : " ");

	mat3_transpose(r_cam_t, r_cam);
	{
		float tmp9[9];
		mat3_mul(tmp9, r_cam, r_delta);
		mat3_mul(r_world, tmp9, r_cam_t);
	}
	printf("R_world:\n");
	for(i = 0; i < 9; i++)
		printf("%.7f%s", r_world[i], (i % 3 == 2) ? "\n" : " ");

	mat4_from_rot3(rot_delta, r_world);
	mat4_from_translation_row(trans_delta, -x * 0.0005f, -z * 0.0005f, y * 0.0005f);

	pivot[0] = (model_extents[0] + model_extents[3]) * 0.5f;
	pivot[1] = (model_extents[1] + model_extents[4]) * 0.5f;
	pivot[2] = (model_extents[2] + model_extents[5]) * 0.5f;
	mat4_from_translation_row(pivot_pos, pivot[0], pivot[1], pivot[2]);
	mat4_from_translation_row(pivot_neg, -pivot[0], -pivot[1], -pivot[2]);

	/* new_affine = trans_delta @ curr_affine @ (pivot_neg @ rot_delta @ pivot_pos) */
	mat4_mul(tmp16a, pivot_neg, rot_delta);
	mat4_mul(tmp16b, tmp16a, pivot_pos);
	mat4_mul(tmp16a, curr_affine, tmp16b);
	mat4_mul(new_affine, trans_delta, tmp16a);

	printf("new_affine flat:\n[");
	for(i = 0; i < 16; i++)
		printf("%.9g%s", new_affine[i], (i < 15) ? ", " : "");
	printf("]\n");

	/* Recorded reference values for this fixture, tolerance for float math. */
	const float expected[16] = {0.928165436f,  -0.364669532f, 0.0743300095f, 0,
								0.0850323066f, 0.402232111f,  0.911580443f,	 0,
								-0.362323582f, -0.839776933f, 0.404346645f,	 0,
								0.0702611357f, -0.238791078f, 0.100086883f,	 1};
	for(i = 0; i < 16; i++)
		assert(fabsf(new_affine[i] - expected[i]) < 1e-5f);
	for(i = 0; i < 3; i++) {
		for(int j = 0; j < 3; j++) {
			float dot = 0;
			for(int k = 0; k < 3; k++)
				dot += r_cam[i * 3 + k] * r_cam[j * 3 + k];
			assert(fabsf(dot - (i == j ? 1.0f : 0.0f)) < 1e-5f);
		}
	}
	return 0;
}
