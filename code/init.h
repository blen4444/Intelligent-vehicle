/*
 * init.h
 *
 *  Created on: 2026年1月12日
 *      Author: DELL
 */

#ifndef CODE_INIT_H_
#define CODE_INIT_H_

#include "zf_common_headfile.h"

extern uint8 camera_init_state;     // 摄像头初始化结果: 0=成功 1=失败

void init(void);

#endif /* CODE_INIT_H_ */
