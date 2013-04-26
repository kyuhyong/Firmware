/*
 * accelerometer_calibration.c
 *
 *   Copyright (C) 2013 Anton Babushkin. All rights reserved.
 *   Author: 	Anton Babushkin	<rk3dov@gmail.com>
 *
 * Transform acceleration vector to true orientation and scale
 *
 * * * * Model * * *
 * accel_corr = accel_T * (accel_raw - accel_offs)
 *
 * accel_corr[3] - fully corrected acceleration vector in body frame
 * accel_T[3][3] - accelerometers transform matrix, rotation and scaling transform
 * accel_raw[3]  - raw acceleration vector
 * accel_offs[3] - acceleration offset vector
 *
 * * * * Calibration * * *
 *
 * Reference vectors
 * accel_corr_ref[6][3] = [  g  0  0 ]     // nose up
 *                        | -g  0  0 |     // nose down
 *                        |  0  g  0 |     // left side down
 *                        |  0 -g  0 |     // right side down
 *                        |  0  0  g |     // on back
 *                        [  0  0 -g ]     // level
 * accel_raw_ref[6][3]
 *
 * accel_corr_ref[i] = accel_T * (accel_raw_ref[i] - accel_offs), i = 0...5
 *
 * 6 reference vectors * 3 axes = 18 equations
 * 9 (accel_T) + 3 (accel_offs) = 12 unknown constants
 *
 * Find accel_offs
 *
 * accel_offs[i] = (accel_raw_ref[i*2][i] + accel_raw_ref[i*2+1][i]) / 2
 *
 *
 * Find accel_T
 *
 * 9 unknown constants
 * need 9 equations -> use 3 of 6 measurements -> 3 * 3 = 9 equations
 *
 * accel_corr_ref[i*2] = accel_T * (accel_raw_ref[i*2] - accel_offs), i = 0...2
 *
 * Solve separate system for each row of accel_T:
 *
 * accel_corr_ref[j*2][i] = accel_T[i] * (accel_raw_ref[j*2] - accel_offs), j = 0...2
 *
 * A * x = b
 *
 * x = [ accel_T[0][i] ]
 *     | accel_T[1][i] |
 *     [ accel_T[2][i] ]
 *
 * b = [ accel_corr_ref[0][i] ]	// One measurement per axis is enough
 *     | accel_corr_ref[2][i] |
 *     [ accel_corr_ref[4][i] ]
 *
 * a[i][j] = accel_raw_ref[i][j] - accel_offs[j], i = 0;2;4, j = 0...2
 *
 * Matrix A is common for all three systems:
 * A = [ a[0][0]  a[0][1]  a[0][2] ]
 *     | a[2][0]  a[2][1]  a[2][2] |
 *     [ a[4][0]  a[4][1]  a[4][2] ]
 *
 * x = A^-1 * b
 *
 * accel_T = A^-1 * g
 * g = 9.80665
 */

#include "accelerometer_calibration.h"

#include <poll.h>
#include <drivers/drv_hrt.h>
#include <uORB/topics/sensor_combined.h>
#include <drivers/drv_accel.h>
#include <systemlib/conversions.h>
#include <mavlink/mavlink_log.h>

void do_accel_calibration(int status_pub, struct vehicle_status_s *status, int mavlink_fd) {
	/* announce change */
	mavlink_log_info(mavlink_fd, "accelerometer calibration started");
	/* set to accel calibration mode */
	status->flag_preflight_accel_calibration = true;
	state_machine_publish(status_pub, status, mavlink_fd);

	float accel_offs_scaled[3];
	float accel_scale[3];
	int res = do_accel_calibration_mesurements(mavlink_fd, accel_offs_scaled, accel_scale);
	if (res == OK) {
		/* measurements complete successfully, set parameters */
		if (param_set(param_find("SENS_ACC_XOFF"), &(accel_offs_scaled[0]))
			|| param_set(param_find("SENS_ACC_YOFF"), &(accel_offs_scaled[1]))
			|| param_set(param_find("SENS_ACC_ZOFF"), &(accel_offs_scaled[2]))
			|| param_set(param_find("SENS_ACC_XSCALE"), &(accel_scale[0]))
			|| param_set(param_find("SENS_ACC_YSCALE"), &(accel_scale[1]))
			|| param_set(param_find("SENS_ACC_ZSCALE"), &(accel_scale[2]))) {
			mavlink_log_critical(mavlink_fd, "Setting offs or scale failed!");
		}

		int fd = open(ACCEL_DEVICE_PATH, 0);
		struct accel_scale ascale = {
			accel_offs_scaled[0],
			accel_scale[0],
			accel_offs_scaled[1],
			accel_scale[1],
			accel_offs_scaled[2],
			accel_scale[2],
		};

		if (OK != ioctl(fd, ACCELIOCSSCALE, (long unsigned int)&ascale))
			warn("WARNING: failed to set scale / offsets for accel");

		close(fd);

		/* auto-save to EEPROM */
		int save_ret = param_save_default();

		if (save_ret != 0) {
			warn("WARNING: auto-save of params to storage failed");
		}

		mavlink_log_info(mavlink_fd, "accel calibration done");

		tune_confirm();
		sleep(2);
		tune_confirm();
		sleep(2);
		/* third beep by cal end routine */
	} else {
		/* measurements error */
		mavlink_log_info(mavlink_fd, "accel calibration aborted");
		tune_error();
		sleep(2);
	}

	/* exit accel calibration mode */
	status->flag_preflight_accel_calibration = false;
	state_machine_publish(status_pub, status, mavlink_fd);
}

int do_accel_calibration_mesurements(int mavlink_fd, float accel_offs_scaled[3], float accel_scale[3]) {
	const int samples_num = 2500;

	int sensor_combined_sub = orb_subscribe(ORB_ID(sensor_combined));

	int16_t accel_raw_ref[6][3];
	bool data_collected[6] = { false, false, false, false, false, false };
	const char *orientation_strs[6] = { "x+", "x-", "y+", "y-", "z+", "z-" };
	while (true) {
		bool done = true;
		char str[80];
		int str_ptr;
		str_ptr = sprintf(str, "keep vehicle still:");
		for (int i = 0; i < 6; i++) {
			if (!data_collected[i]) {
				str_ptr += sprintf(&(str[str_ptr]), " %s", orientation_strs[i]);
				done = false;
			}
		}
		if (done) {
			mavlink_log_info(mavlink_fd, "all accel measurements complete");
			break;
		} else {
			mavlink_log_info(mavlink_fd, str);
			int orient = detect_orientation(mavlink_fd, sensor_combined_sub);
			if (orient < 0) {
				sprintf(str, "orientation detection error: %i", orient);
				mavlink_log_info(mavlink_fd, str);
				return ERROR;
			}
			mavlink_log_info(mavlink_fd, "accel measurement started");
			read_accelerometer_raw_avg(sensor_combined_sub, &(accel_raw_ref[orient][0]), samples_num);
			//mavlink_log_info(mavlink_fd, "accel measurement complete");
			str_ptr = sprintf(str, "complete: %i [ %i %i %i ]", orient, accel_raw_ref[orient][0], accel_raw_ref[orient][1], accel_raw_ref[orient][2]);
			mavlink_log_info(mavlink_fd, str);
			data_collected[orient] = true;
			tune_confirm();
		}
	}
	close(sensor_combined_sub);

	/* calculate offsets and rotation+scale matrix */
	int16_t accel_offs[3];
	float accel_T[3][3];
	int res = calculate_calibration_values(accel_raw_ref, accel_T, accel_offs, CONSTANTS_ONE_G);
	if (res != 0) {
		mavlink_log_info(mavlink_fd, "calibration values calculation error");
		return ERROR;
	}

	char str[80];
	sprintf(str, "accel offsets: [ %i  %i  %i ]", accel_offs[0], accel_offs[1], accel_offs[2]);
	mavlink_log_info(mavlink_fd, str);
	//mavlink_log_info(mavlink_fd, "accel transform matrix:");
	for (int i = 0; i < 3; i++) {
		//sprintf(str, "\t[ %0.6f  %0.6f  %0.6f ]", accel_T[i][0], accel_T[i][1], accel_T[i][2]);
		//mavlink_log_info(mavlink_fd, str);
	}

	/* convert raw accel offset to scaled and transform matrix to scales
	 * rotation part of transform matrix is not used by now */
	for (int i = 0; i < 3; i++) {
		accel_offs_scaled[i] = accel_offs[i] * accel_T[i][i];
		accel_scale[i] = accel_T[i][i];
	}

	return OK;
}

/*
 * Wait for vehicle become still and detect it's orientation.
 *
 * @return 0..5 according to orientation when vehicle is still and ready for measurements,
 * ERROR if vehicle is not still after 10s or orientation error is more than 20%
 */
int detect_orientation(int mavlink_fd, int sub_sensor_combined) {
	struct sensor_combined_s sensor;
	/* exponential moving average of accel */
	float accel_ema[3] = { 0.0f, 0.0f, 0.0f };
	/* max-hold dispersion of accel */
	float accel_disp[3] = { 0.0f, 0.0f, 0.0f };
	float accel_len2 = 0.0f;
	/* EMA time constant in seconds*/
	float ema_len = 0.2f;
	/* set "still" threshold to 0.005 m/s^2 */
	float still_thr2 = pow(0.05f / CONSTANTS_ONE_G, 2);
	/* set accel error threshold to 20% of accel vector length */
	float accel_err_thr = 0.2f;
	/* still time required in us */
	int64_t still_time = 2000000;
	struct pollfd fds[1] = { { .fd = sub_sensor_combined, .events = POLLIN } };

	hrt_abstime t_start = hrt_absolute_time();
	/* set deadline to 20s */
	hrt_abstime timeout = 20000000;
	hrt_abstime t_timeout = t_start + timeout;
	hrt_abstime t = t_start;
	hrt_abstime t_prev = t_start;
	hrt_abstime t_still = 0;
	while (true) {
		/* wait blocking for new data */
		int poll_ret = poll(fds, 1, 1000);
		if (poll_ret) {
			orb_copy(ORB_ID(sensor_combined), sub_sensor_combined, &sensor);
			t = hrt_absolute_time();
			float dt = (t - t_prev) / 1000000.0f;
			t_prev = t;
			float w = dt / ema_len;
			for (int i = 0; i < 3; i++) {
				accel_ema[i] = accel_ema[i] * (1.0f - w) + sensor.accelerometer_raw[i] * w;
				float d = (float) sensor.accelerometer_raw[i] - accel_ema[i];
				d = d * d;
				accel_disp[i] = accel_disp[i] * (1.0f - w);
				if (d > accel_disp[i])
					accel_disp[i] = d;
			}
			accel_len2 = accel_ema[0] * accel_ema[0] + accel_ema[1] * accel_ema[1] + accel_ema[2] * accel_ema[2];
			float still_thr_raw2 = still_thr2 * accel_len2;
			if (  accel_disp[0] < still_thr_raw2 &&
				  accel_disp[1] < still_thr_raw2 &&
				  accel_disp[2] < still_thr_raw2 ) {
				/* is still now */
				if (t_still == 0) {
					/* first time */
					mavlink_log_info(mavlink_fd, "still");
					t_still = t;
					t_timeout = t + timeout;
				} else {
					/* still since t_still */
					if ((int64_t) t - (int64_t) t_still > still_time) {
						/* vehicle is still, exit from the loop to detection of its orientation */
						break;
					}
				}
			} else if ( accel_disp[0] > still_thr_raw2 * 2.0f ||
					    accel_disp[1] > still_thr_raw2 * 2.0f ||
					    accel_disp[2] > still_thr_raw2 * 2.0f) {
				/* not still, reset still start time */
				if (t_still != 0) {
					mavlink_log_info(mavlink_fd, "moving");
					t_still = 0;
				}
			}
		} else if (poll_ret == 0) {
			/* any poll failure for 1s is a reason to abort */
			mavlink_log_info(mavlink_fd, "ERROR: poll failure");
			return -3;
		}
		if (t > t_timeout) {
			mavlink_log_info(mavlink_fd, "ERROR: timeout");
			return -1;
		}
	}
	float accel_len = sqrt(accel_len2);
	float accel_err_thr_raw = accel_len * accel_err_thr;
	char str[80];
	sprintf(str, "ema: [ %.1f  %.1f  %.1f ]", accel_ema[0], accel_ema[1], accel_ema[2]);
	mavlink_log_info(mavlink_fd, str);
	sprintf(str, "disp: [ %.1f  %.1f  %.1f ]", accel_disp[0], accel_disp[1], accel_disp[2]);
	mavlink_log_info(mavlink_fd, str);
	if (  fabs(accel_ema[0] - accel_len) < accel_err_thr_raw &&
		  fabs(accel_ema[1]) < accel_err_thr_raw &&
		  fabs(accel_ema[2]) < accel_err_thr_raw  )
		return 0;	// [ g, 0, 0 ]
	if (  fabs(accel_ema[0] + accel_len) < accel_err_thr_raw &&
		  fabs(accel_ema[1]) < accel_err_thr_raw &&
		  fabs(accel_ema[2]) < accel_err_thr_raw  )
		return 1;	// [ -g, 0, 0 ]
	if (  fabs(accel_ema[0]) < accel_err_thr_raw &&
		  fabs(accel_ema[1] - accel_len) < accel_err_thr_raw &&
		  fabs(accel_ema[2]) < accel_err_thr_raw  )
		return 2;	// [ 0, g, 0 ]
	if (  fabs(accel_ema[0]) < accel_err_thr_raw &&
		  fabs(accel_ema[1] + accel_len) < accel_err_thr_raw &&
		  fabs(accel_ema[2]) < accel_err_thr_raw  )
		return 3;	// [ 0, -g, 0 ]
	if (  abs(accel_ema[0]) < accel_err_thr_raw &&
		  abs(accel_ema[1]) < accel_err_thr_raw &&
		  abs(accel_ema[2] - accel_len) < accel_err_thr_raw  )
		return 4;	// [ 0, 0, g ]
	if (  abs(accel_ema[0]) < accel_err_thr_raw &&
		  abs(accel_ema[1]) < accel_err_thr_raw &&
		  abs(accel_ema[2] + accel_len) < accel_err_thr_raw  )
		return 5;	// [ 0, 0, -g ]
	mavlink_log_info(mavlink_fd, "ERROR: invalid orientation");
	return -2;	// Can't detect orientation
}

/*
 * Read specified number of accelerometer samples, calculate average and dispersion.
 */
int read_accelerometer_raw_avg(int sensor_combined_sub, int16_t accel_avg[3], int samples_num) {
	struct pollfd fds[1] = { { .fd = sensor_combined_sub, .events = POLLIN } };
	int count = 0;
	int32_t accel_sum[3] = { 0, 0, 0 };
	while (count < samples_num) {
		int poll_ret = poll(fds, 1, 1000);
		if (poll_ret == 1) {
			struct sensor_combined_s sensor;
			orb_copy(ORB_ID(sensor_combined), sensor_combined_sub, &sensor);
			for (int i = 0; i < 3; i++) {
				accel_sum[i] += sensor.accelerometer_raw[i];
			}
			count++;
		} else {
			return ERROR;
		}
	}
	for (int i = 0; i < 3; i++) {
		accel_avg[i] = (accel_sum[i] + count / 2) / count;
	}
	/* calculate dispersion */
	return OK;
}

/*
 * Convert raw values from accelerometers to m/s^2.
 */
void acceleration_raw_to_m_s2(float accel_corr[3], int16_t accel_raw[3],
		float accel_T[3][3], int16_t accel_offs[3]) {
	for (int i = 0; i < 3; i++) {
		accel_corr[i] = 0.0f;
		for (int j = 0; j < 3; j++) {
			accel_corr[i] += accel_T[i][j] * (accel_raw[j] - accel_offs[j]);
		}
	}
}

int mat_invert3(float src[3][3], float dst[3][3]) {
	float det = src[0][0] * (src[1][1] * src[2][2] - src[1][2] * src[2][1]) -
			src[0][1] * (src[1][0] * src[2][2] - src[1][2] * src[2][0]) +
			src[0][2] * (src[1][0] * src[2][1] - src[1][1] * src[2][0]);
	if (det == 0.0)
		return -1;	// Singular matrix
	dst[0][0] = (src[1][1] * src[2][2] - src[1][2] * src[2][1]) / det;
	dst[1][0] = (src[1][2] * src[2][0] - src[1][0] * src[2][2]) / det;
	dst[2][0] = (src[1][0] * src[2][1] - src[1][1] * src[2][0]) / det;
	dst[0][1] = (src[0][2] * src[2][1] - src[0][1] * src[2][2]) / det;
	dst[1][1] = (src[0][0] * src[2][2] - src[0][2] * src[2][0]) / det;
	dst[2][1] = (src[0][1] * src[2][0] - src[0][0] * src[2][1]) / det;
	dst[0][2] = (src[0][1] * src[1][2] - src[0][2] * src[1][1]) / det;
	dst[1][2] = (src[0][2] * src[1][0] - src[0][0] * src[1][2]) / det;
	dst[2][2] = (src[0][0] * src[1][1] - src[0][1] * src[1][0]) / det;
	return 0;
}

int calculate_calibration_values(int16_t accel_raw_ref[6][3],
		float accel_T[3][3], int16_t accel_offs[3], float g) {
	/* calculate raw offsets */
	for (int i = 0; i < 3; i++) {
		accel_offs[i] = (int16_t) (((int32_t) (accel_raw_ref[i * 2][i])
				+ (int32_t) (accel_raw_ref[i * 2 + 1][i])) / 2);
	}
	/* fill matrix A for linear equations system*/
	float mat_A[3][3];
	memset(mat_A, 0, sizeof(mat_A));
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			float a = (accel_raw_ref[i * 2][j] - accel_offs[j]);
			mat_A[i][j] = a;
		}
	}
	/* calculate inverse matrix for A */
	float mat_A_inv[3][3];
	mat_invert3(mat_A, mat_A_inv);
	for (int i = 0; i < 3; i++) {
		/* copy results to accel_T */
		for (int j = 0; j < 3; j++) {
			/* simplify matrices mult because b has only one non-zero element == g at index i */
			accel_T[j][i] = mat_A_inv[j][i] * g;
		}
	}
	return 0;
}
