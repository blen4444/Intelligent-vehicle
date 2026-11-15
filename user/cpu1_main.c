/*********************************************************************************************************************
* TC264 Opensourec Library 即（TC264 开源库）是一个基于官方 SDK 接口的第三方开源库
* Copyright (c) 2022 SEEKFREE 逐飞科技
*
* 本文件是 TC264 开源库的一部分
*
* TC264 开源库 是免费软件
* 您可以根据自由软件基金会发布的 GPL（GNU General Public License，即 GNU通用公共许可证）的条款
* 即 GPL 的第3版（即 GPL3.0）或（您选择的）任何后来的版本，重新发布和/或修改它
*
* 本开源库的发布是希望它能发挥作用，但并未对其作任何的保证
* 甚至没有隐含的适销性或适合特定用途的保证
* 更多细节请参见 GPL
*
* 您应该在收到本开源库的同时收到一份 GPL 的副本
* 如果没有，请参阅<https://www.gnu.org/licenses/>
*
* 额外注明：
* 本开源库使用 GPL3.0 开源许可证协议 以上许可申明为译文版本
* 许可申明英文版在 libraries/doc 文件夹下的 GPL3_permission_statement.txt 文件中
* 许可证副本在 libraries 文件夹下 即该文件夹下的 LICENSE 文件
* 欢迎各位使用并传播本程序 但修改内容时必须保留逐飞科技的版权声明（即本声明）
*
* 文件名称          cpu1_main
* 公司名称          成都逐飞科技有限公司
* 版本信息          查看 libraries/doc 文件夹内 version 文件 版本说明
* 开发环境          ADS v1.9.4
* 适用平台          TC264D
* 店铺链接          https://seekfree.taobao.com/
*
* 修改记录
* 日期              作者                备注
* 2022-09-15       pudding            first version
********************************************************************************************************************/

#include "zf_common_headfile.h"
#include "vision_runtime.h"
#include "image.h"
#include "control.h"
#include "IfxStm.h"

#pragma section all "cpu1_dsram"

// CPU1 专用: 视觉处理(取图 -> 图像处理 -> 元素识别/状态机 -> 发布结果)。
// 摄像头采集的 VSYNC/DMA 中断仍在 CPU0, CPU1 只轮询 mt9v03x_finish_flag 并干重活。

static uint32 vision_elapsed_us(uint32 begin, uint32 end, uint32 stm_hz)
{
    // 先做 32 位无符号减法, 再换算: 跨一次 STM 回绕也不会错
    return (uint32)(((uint64)(uint32)(end - begin) * 1000000u) / stm_hz);
}

void core1_main(void)
{
    uint32 stm_hz;
    uint32 last_result_stm = 0;
    uint8  have_result_stm = 0;

    disable_Watchdog();                     // 关闭看门狗
    IfxCpu_setDataCache(0);                 // 双核共享数据, 关闭数据缓存避免不一致
    cpu_wait_event_ready();                 // 等 CPU0 完成摄像头/中断初始化(此时中断未开, 延时用 STM 轮询)
    interrupt_global_enable(0);             // 打开全局中断

    stm_hz = (uint32)IfxStm_getFrequency(&MODULE_STM0);
    if(stm_hz == 0u) stm_hz = 1u;

    while (TRUE)
    {
        // ==================== 视觉: 一帧处理 ====================
        if(mt9v03x_finish_flag)
        {
            uint32 process_begin = IfxStm_getLower(&MODULE_STM0);
            uint32 result_stm;
            float previous_mid = last_mid_line;
            float next_mid;

            // 先抢缓冲再做检查: VSYNC 若已赢下竞争会清掉标志, 这帧丢弃重试;
            // 之后的 VSYNC 看到锁就会跳过启动 DMA, 不会覆盖正在拷贝的图。
            camera_copy_lock = 1;
            if(!mt9v03x_finish_flag)
            {
                camera_copy_lock = 0;
                vision_copy_reject_count++;
                continue;
            }
            mt9v03x_finish_flag = 0;
            memcpy(base_image[0], mt9v03x_image[0], MT9V03X_IMAGE_SIZE);
            camera_copy_lock = 0;

            img_threshold = Ostuf(base_image[0]);
            set_image_twovalues(img_threshold);
            find_jidian(image);
            image_deal(image);
            next_mid = find_mid_line_weight();

            // NaN 同样过不了比较; 结果非法时保留旧中线, 不能当成新帧
            if(!(next_mid >= 1.0f && next_mid <= (float)(MT9V03X_W - 2)))
            {
                last_mid_line = previous_mid;
                vision_result_reject_count++;
                continue;
            }
            final_mid_line = next_mid;
            control_publish_vision(final_mid_line, mid_line_list,
                                   left_line_list, right_line_list);

            result_stm = IfxStm_getLower(&MODULE_STM0);
            vision_process_us = vision_elapsed_us(process_begin, result_stm, stm_hz);
            if(have_result_stm)
                vision_result_interval_us = vision_elapsed_us(last_result_stm, result_stm, stm_hz);
            last_result_stm = result_stm;
            have_result_stm = 1;
            vision_processed_count++;
        }
    }
}

#pragma section all restore
