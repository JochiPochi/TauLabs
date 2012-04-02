/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Sensors
 * @brief Acquires sensor data 
 * Specifically updates the the @ref Gyros, @ref Accels, and @ref Magnetometer objects
 * @{
 *
 * @file       sensors.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref Gyros @ref Accels @ref Magnetometer
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "attitude.h"
#include "magnetometer.h"
#include "accels.h"
#include "gyros.h"
#include "gyrosbias.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "revocalibration.h"
#include "flightstatus.h"
#include "gpsposition.h"
#include "baroaltitude.h"
#include "CoordinateConversions.h"

#include <pios_board_info.h>

// Private constants
#define STACK_SIZE_BYTES 1540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)
#define SENSOR_PERIOD 2

#define F_PI 3.14159265358979323846f
#define PI_MOD(x) (fmodf(x + F_PI, F_PI * 2) - F_PI)
// Private types

// Private variables
static xTaskHandle sensorsTaskHandle;
static bool gps_updated = false;
static bool baro_updated = false;

// Private functions
static void SensorsTask(void *parameters);
static void settingsUpdatedCb(UAVObjEvent * objEv);
static void sensorsUpdatedCb(UAVObjEvent * objEv);

// These values are initialized by settings but can be updated by the attitude algorithm
static bool bias_correct_gyro = true;

static float mag_bias[3] = {0,0,0};
static float mag_scale[3] = {0,0,0};
static float accel_bias[3] = {0,0,0};
static float accel_scale[3] = {0,0,0};

static float R[3][3] = {{0}};
static int8_t rotate = 0;

/**
 * API for sensor fusion algorithms:
 * Configure(xQueueHandle gyro, xQueueHandle accel, xQueueHandle mag, xQueueHandle baro)
 *   Stores all the queues the algorithm will pull data from
 * FinalizeSensors() -- before saving the sensors modifies them based on internal state (gyro bias)
 * Update() -- queries queues and updates the attitude estiamte
 */


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsInitialize(void)
{
	GyrosInitialize();
	GyrosBiasInitialize();
	AccelsInitialize();
	MagnetometerInitialize();
	RevoCalibrationInitialize();
	AttitudeSettingsInitialize();

	rotate = 0;

	RevoCalibrationConnectCallback(&settingsUpdatedCb);
	AttitudeSettingsConnectCallback(&settingsUpdatedCb);

	return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsStart(void)
{
	// Start main task
	xTaskCreate(SensorsTask, (signed char *)"Sensors", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &sensorsTaskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_SENSORS, sensorsTaskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_SENSORS);

	return 0;
}

MODULE_INITCALL(SensorsInitialize, SensorsStart)

int32_t accel_test;
int32_t gyro_test;
int32_t mag_test;
//int32_t pressure_test;


/**
 * The sensor task.  This polls the gyros at 500 Hz and pumps that data to
 * stabilization and to the attitude loop
 * 
 * This function has a lot of if/defs right now to allow these configurations:
 * 1. BMA180 accel and MPU6000 gyro
 * 2. MPU6000 gyro and accel
 * 3. BMA180 accel and L3GD20 gyro
 */

uint32_t sensor_dt_us;
static void SensorsTask(void *parameters)
{
	portTickType lastSysTime;
	uint32_t accel_samples;
	uint32_t gyro_samples;
	int32_t accel_accum[3] = {0, 0, 0};
	int32_t gyro_accum[3] = {0,0,0};
	float gyro_scaling = 0;
	float accel_scaling = 0;
	static int32_t timeval;

	AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);

	UAVObjEvent ev;
	settingsUpdatedCb(&ev);

	const struct pios_board_info * bdinfo = &pios_board_info_blob;	

	switch(bdinfo->board_rev) {
		case 0x01:
#if defined(PIOS_INCLUDE_L3GD20)
			gyro_test = PIOS_L3GD20_Test();
#endif
#if defined(PIOS_INCLUDE_BMA180)
			accel_test = PIOS_BMA180_Test();
#endif
			break;
		case 0x02:
#if defined(PIOS_INCLUDE_MPU6000)
			gyro_test = PIOS_MPU6000_Test();
			accel_test = gyro_test;
#endif
			break;
		default:
			PIOS_DEBUG_Assert(0);
	}

#if defined(PIOS_INCLUDE_HMC5883)
	mag_test = PIOS_HMC5883_Test();
#endif

	if(accel_test < 0 || gyro_test < 0 || mag_test < 0) {
		AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
		while(1) {
			PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
			vTaskDelay(10);
		}
	}
	
	// If debugging connect callback
	if(pios_com_aux_id != 0) {
		BaroAltitudeConnectCallback(&sensorsUpdatedCb);
		GPSPositionConnectCallback(&sensorsUpdatedCb);
	}
	
	// Main task loop
	lastSysTime = xTaskGetTickCount();
	bool error = false;
	uint32_t mag_update_time = PIOS_DELAY_GetRaw();
	while (1) {
		// TODO: add timeouts to the sensor reads and set an error if the fail
		sensor_dt_us = PIOS_DELAY_DiffuS(timeval);
		timeval = PIOS_DELAY_GetRaw();

		if (error) {
			PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
			lastSysTime = xTaskGetTickCount();
			vTaskDelayUntil(&lastSysTime, SENSOR_PERIOD / portTICK_RATE_MS);
			AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
			error = false;
		} else {
			AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);
		}

		int32_t read_good;
		int32_t count;

		for (int i = 0; i < 3; i++) {
			accel_accum[i] = 0;
			gyro_accum[i] = 0;
		}
		accel_samples = 0;
		gyro_samples = 0;

		AccelsData accelsData;
		GyrosData gyrosData;

		switch(bdinfo->board_rev) {
			case 0x01:  // L3GD20 + BMA180 board
#if defined(PIOS_INCLUDE_BMA180)
			{
				struct pios_bma180_data accel;
				
				count = 0;
				while((read_good = PIOS_BMA180_ReadFifo(&accel)) != 0 && !error)
					error = ((xTaskGetTickCount() - lastSysTime) > SENSOR_PERIOD) ? true : error;
				if (error) {
					// Unfortunately if the BMA180 ever misses getting read, then it will not
					// trigger more interrupts.  In this case we must force a read to kickstarts
					// it.
					struct pios_bma180_data data;
					PIOS_BMA180_ReadAccels(&data);
					continue;
				}
				while(read_good == 0) {	
					count++;
					
					accel_accum[0] += accel.x;
					accel_accum[1] += accel.y;
					accel_accum[2] += accel.z;
					
					read_good = PIOS_BMA180_ReadFifo(&accel);
				}
				accel_samples = count;
				accel_scaling = PIOS_BMA180_GetScale();
				
				// Get temp from last reading
				accelsData.temperature = 25.0f + ((float) accel.temperature - 2.0f) / 2.0f;
			}
#endif
#if defined(PIOS_INCLUDE_L3GD20)
			{
				struct pios_l3gd20_data gyro;
				gyro_samples = 0;
				xQueueHandle gyro_queue = PIOS_L3GD20_GetQueue();
				
				if(xQueueReceive(gyro_queue, (void *) &gyro, 4) == errQUEUE_EMPTY) {
					error = true;
					continue;
				}
				
				gyro_samples = 1;
				gyro_accum[0] += gyro.gyro_x;
				gyro_accum[1] += gyro.gyro_y;
				gyro_accum[2] += gyro.gyro_z;
				
				gyro_scaling = PIOS_L3GD20_GetScale();

				// Get temp from last reading
				gyrosData.temperature = gyro.temperature;
			}
#endif
				break;
			case 0x02:  // MPU6000 board
#if defined(PIOS_INCLUDE_MPU6000)
			{
				struct pios_mpu6000_data gyro;
				
				count = 0;
				while((read_good = PIOS_MPU6000_ReadFifo(&gyro)) != 0 && !error)
					error = ((xTaskGetTickCount() - lastSysTime) > SENSOR_PERIOD) ? true : error;
				if (error)
					continue;
				while(read_good == 0) {
					count++;
					
					gyro_accum[0] += gyro.gyro_x;
					gyro_accum[1] += gyro.gyro_y;
					gyro_accum[2] += gyro.gyro_z;
					
					accel_accum[0] += gyro.accel_x;
					accel_accum[1] += gyro.accel_y;
					accel_accum[2] += gyro.accel_z;
					
					read_good = PIOS_MPU6000_ReadFifo(&gyro);
				}
				gyro_samples = count;
				gyro_scaling = PIOS_MPU6000_GetScale();
				
				accel_samples = count;
				accel_scaling = PIOS_MPU6000_GetAccelScale();

				// Get temp from last reading
				gyrosData.temperature = 35.0f + ((float) gyro.temperature + 512.0f) / 340.0f;
				accelsData.temperature = 35.0f + ((float) gyro.temperature + 512.0f) / 340.0f;
			}
#endif /* PIOS_INCLUDE_MPU6000 */
				break;
			default:
				PIOS_DEBUG_Assert(0);
		}

		// Scale the accels
		float accels[3] = {(float) accel_accum[1] / accel_samples, 
		                   (float) accel_accum[0] / accel_samples,
		                  -(float) accel_accum[2] / accel_samples};
		float accels_out[3] = {accels[0] * accel_scaling * accel_scale[0] - accel_bias[0],
		                       accels[1] * accel_scaling * accel_scale[1] - accel_bias[1],
		                       accels[2] * accel_scaling * accel_scale[2] - accel_bias[2]};
		if (rotate) {
			rot_mult(R, accels_out, accels);
			accelsData.x = accels[0];
			accelsData.y = accels[1];
			accelsData.z = accels[2];
		} else {
			accelsData.x = accels_out[0];
			accelsData.y = accels_out[1];
			accelsData.z = accels_out[2];
		}
		AccelsSet(&accelsData);

		// Scale the gyros
		float gyros[3] = {(float) gyro_accum[1] / gyro_samples,
		                  (float) gyro_accum[0] / gyro_samples,
		                 -(float) gyro_accum[2] / gyro_samples};
		float gyros_out[3] = {gyros[0] * gyro_scaling,
		                      gyros[1] * gyro_scaling,
		                      gyros[2] * gyro_scaling};
		if (rotate) {
			rot_mult(R, gyros_out, gyros);
			gyrosData.x = gyros[0];
			gyrosData.y = gyros[1];
			gyrosData.z = gyros[2];
		} else {
			gyrosData.x = gyros_out[0];
			gyrosData.y = gyros_out[1];
			gyrosData.z = gyros_out[2];
		}
		
		if (bias_correct_gyro) {
			// Apply bias correction to the gyros
			GyrosBiasData gyrosBias;
			GyrosBiasGet(&gyrosBias);
			gyrosData.x += gyrosBias.x;
			gyrosData.y += gyrosBias.y;
			gyrosData.z += gyrosBias.z;
		}
		GyrosSet(&gyrosData);
		
		// Because most crafts wont get enough information from gravity to zero yaw gyro, we try
		// and make it average zero (weakly)

#if defined(PIOS_INCLUDE_HMC5883)
		MagnetometerData mag;
		if (PIOS_HMC5883_NewDataAvailable() || PIOS_DELAY_DiffuS(mag_update_time) > 150000) {
			int16_t values[3];
			PIOS_HMC5883_ReadMag(values);
			float mags[3] = {(float) values[1] * mag_scale[0] - mag_bias[0],
			                (float) values[0] * mag_scale[1] - mag_bias[1],
			                -(float) values[2] * mag_scale[2] - mag_bias[2]};
			if (rotate) {
				float mag_out[3];
				rot_mult(R, mags, mag_out);
				mag.x = mag_out[0];
				mag.y = mag_out[1];
				mag.z = mag_out[2];
			} else {
				mag.x = mags[0];
				mag.y = mags[1];
				mag.z = mags[2];
			}
			MagnetometerSet(&mag);
			mag_update_time = PIOS_DELAY_GetRaw();
		}
#endif

		PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);

		switch(bdinfo->board_rev) {
			case 0x01:  // L3GD20 + BMA180 board
				lastSysTime = xTaskGetTickCount();
				break;
			case 0x02:
				vTaskDelayUntil(&lastSysTime, SENSOR_PERIOD / portTICK_RATE_MS);
				break;
			default:
				PIOS_DEBUG_Assert(0);
		}
	}
}

/**
 * Indicate that these sensors have been updated
 */
static void sensorsUpdatedCb(UAVObjEvent * objEv)
{
	if(objEv->obj == GPSPositionHandle())
		gps_updated = true;
	if(objEv->obj == BaroAltitudeHandle())
		baro_updated = true;
}

/**
 * Locally cache some variables from the AtttitudeSettings object
 */
static void settingsUpdatedCb(UAVObjEvent * objEv) {
	RevoCalibrationData cal;
	RevoCalibrationGet(&cal);
	
	mag_bias[0] = cal.mag_bias[REVOCALIBRATION_MAG_BIAS_X];
	mag_bias[1] = cal.mag_bias[REVOCALIBRATION_MAG_BIAS_Y];
	mag_bias[2] = cal.mag_bias[REVOCALIBRATION_MAG_BIAS_Z];
	mag_scale[0] = cal.mag_scale[REVOCALIBRATION_MAG_SCALE_X];
	mag_scale[1] = cal.mag_scale[REVOCALIBRATION_MAG_SCALE_Y];
	mag_scale[2] = cal.mag_scale[REVOCALIBRATION_MAG_SCALE_Z];
	accel_bias[0] = cal.accel_bias[REVOCALIBRATION_ACCEL_BIAS_X];
	accel_bias[1] = cal.accel_bias[REVOCALIBRATION_ACCEL_BIAS_Y];
	accel_bias[2] = cal.accel_bias[REVOCALIBRATION_ACCEL_BIAS_Z];
	accel_scale[0] = cal.accel_scale[REVOCALIBRATION_ACCEL_SCALE_X];
	accel_scale[1] = cal.accel_scale[REVOCALIBRATION_ACCEL_SCALE_Y];
	accel_scale[2] = cal.accel_scale[REVOCALIBRATION_ACCEL_SCALE_Z];

	AttitudeSettingsData attitudeSettings;
	AttitudeSettingsGet(&attitudeSettings);

	// Indicates not to expend cycles on rotation
	if(attitudeSettings.BoardRotation[0] == 0 && attitudeSettings.BoardRotation[1] == 0 &&
	   attitudeSettings.BoardRotation[2] == 0) {
		rotate = 0;
	} else {
		float rotationQuat[4];
		const float rpy[3] = {attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_ROLL],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_PITCH],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_YAW]};
		RPY2Quaternion(rpy, rotationQuat);
		Quaternion2R(rotationQuat, R);
		rotate = 1;
	}

}
/**
  * @}
  * @}
  */