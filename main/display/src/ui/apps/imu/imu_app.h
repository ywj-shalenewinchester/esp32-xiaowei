#pragma once

#include "../../framework/app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IMU 测试页（测试 → IMU）。
 * 一屏对称滑条：姿态角 PITCH/ROLL、加速度 AX/AY/AZ、角速度 GX/GY/GZ。 */
const xiaowei_app_t *imu_app_descriptor(void);

#ifdef __cplusplus
}
#endif
