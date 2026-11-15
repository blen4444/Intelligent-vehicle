/*********************************************************************************************************************
* 文件名称          cpu0_main
* 功能              CPU0 主循环: 屏幕显示 / 状态 / 调参菜单 (视觉处理在 CPU1)
* 修改记录
* 日期              作者                备注
* 2022-09-15       pudding            first version
********************************************************************************************************************/
#include "zf_common_headfile.h"
#include "vision_runtime.h"
#include "IfxStm.h"

#pragma section all "cpu0_dsram"
// 本语句与文末的 #pragma section all restore 之间的全局变量都放在 CPU0 的 RAM 中

// 视觉运行时统计量(中断与主循环都会写, 供屏幕显示)
volatile uint8  camera_copy_lock = 0;          // 1 = 主循环正在 memcpy, 此时 VSYNC 跳过本次采集
volatile uint32 vision_skipped_capture_count = 0;
volatile uint32 camera_capture_epoch = 0;
volatile uint32 camera_frame_count = 0;
volatile uint32 vision_processed_count = 0;
volatile uint32 vision_copy_reject_count = 0;
volatile uint32 vision_result_reject_count = 0;
volatile uint16 vision_camera_fps = 0;
volatile uint16 vision_process_fps = 0;
volatile uint32 vision_process_us = 0;
volatile uint32 vision_result_interval_us = 0;

static uint16 vision_window_fps(uint32 frames, uint32 elapsed_ticks)
{
    // 5ms 一拍: frames * (1000ms / 5ms) / ticks = frames * 200 / ticks
    uint32 fps = (uint32)(((uint64)frames * 200u) / elapsed_ticks);
    return (uint16)(fps > 999u ? 999u : fps);
}

//====================== 第 1 页(视觉/巡线)标签 ======================
static void vision_status_labels(void)
{
    ips200_show_string(5, 160, "final_mid_line");
    ips200_show_string(180, 160, "T:");
    ips200_show_string(0, 185, "cam:");
    ips200_show_string(90, 185, "fr:");
    ips200_show_string(0, 205, "start:");
    ips200_show_string(100, 205, "base:");
    ips200_show_string(0, 225, "L:");
    ips200_show_string(110, 225, "R:");
    ips200_show_string(0, 245, "camFPS:");
    ips200_show_string(112, 245, "proc:");
    ips200_show_string(0, 265, "us:");
    ips200_show_string(112, 265, "dt:");
    ips200_show_string(0, 285, "drop:");
    ips200_show_string(112, 285, "PIT:");
    ips200_show_string(0, 304, "zt:");
    ips200_show_string(100, 304, "sg:");
}

//====================== 第 2 页(编码器/闭环)标签 ======================
static void enc_status_labels(void)
{
    ips200_show_string(0, 160, "cnt L:");
    ips200_show_string(108, 160, "R:");
    ips200_show_string(0, 185, "tgt L:");
    ips200_show_string(108, 185, "R:");
    ips200_show_string(0, 205, "duty L:");
    ips200_show_string(115, 205, "R:");
    ips200_show_string(0, 225, "k L:");
    ips200_show_string(80, 225, "R:");
    ips200_show_string(145, 225, "loop:");
    ips200_show_string(0, 245, "fault:");
    ips200_show_string(80, 245, "base:");
    ips200_show_string(145, 245, "bend:");
}

int core0_main(void)
{
    uint32 last_status_tick = 0;
    uint32 last_preview_tick = 0;
    uint32 last_camera_count = 0;
    uint32 last_process_count = 0;
    uint8 last_preview_mode = 2;
    uint8 labels_page = 0xFF;            // 已经画好标签的页号
    uint8 tune_menu_last_version = 0;

    clock_init();                        // 获取时钟频率<务必保留>
    IfxCpu_setDataCache(0);              // 双核共享数据, 关闭数据缓存避免不一致
    debug_init();                        // 初始化默认调试串口

    init();                              // 各模块初始化(摄像头/舵机/电机/中断)
    ips200_init(IPS200_TYPE_SPI);
    cpu_wait_event_ready();              // 通知 CPU1 初始化完成, 并等 CPU1 就绪
    last_status_tick = sys_tick;
    last_preview_tick = sys_tick;
    last_camera_count = camera_frame_count;

    while (TRUE)
    {
        uint32 now_tick;
        uint8 running;

        // 视觉处理已移到 CPU1(core1_main), 这里只负责屏幕显示 / 状态 / 菜单

        now_tick = sys_tick;
        running = (uint8)(start & 1u);
        if(running != last_preview_mode)
        {
            last_preview_mode = running;
            // 发车瞬间不要整屏重画
#if !VISION_RUN_PREVIEW_ENABLE
            if(running)
                ips200_show_string(0, 0, "RUN: PREVIEW PAUSED");
#endif
            last_preview_tick = now_tick;
        }

        // 刷图是诊断工作, 不参与视觉节拍: 跑动 5Hz, 停车 10Hz
        if(vision_processed_count &&
           ((!running && now_tick - last_preview_tick >= VISION_STOP_PREVIEW_TICKS)
#if VISION_RUN_PREVIEW_ENABLE
            || (running && now_tick - last_preview_tick >= VISION_RUN_PREVIEW_TICKS)
#endif
           ))
        {
            last_preview_tick = now_tick;
            ips200_show_gray_image(0, 0, image[0], MT9V03X_W, MT9V03X_H, 240, 153, 0);
            draw_line();
        }

        // ==================== 状态数值: 500ms 刷一次 ====================
        if(now_tick - last_status_tick >= VISION_STATUS_TICKS)
        {
            uint32 elapsed = now_tick - last_status_tick;
            uint32 camera_count = camera_frame_count;
            uint32 process_count = vision_processed_count;
            uint32 shown_process_us = vision_process_us;
            uint32 shown_interval_us = vision_result_interval_us;
            vision_camera_fps = vision_window_fps(camera_count - last_camera_count, elapsed);
            vision_process_fps = vision_window_fps(process_count - last_process_count, elapsed);
            last_camera_count = camera_count;
            last_process_count = process_count;
            last_status_tick = now_tick;

            if(labels_page != status_page)   // KEY_4 换页: 整屏清一次再重画标签
            {
                labels_page = status_page;
                ips200_clear();
                if(status_page == 0)      vision_status_labels();
                else if(status_page == 1) enc_status_labels();
                else
                {
                    tune_menu_last_version = tune_menu_version;
                    tune_menu_draw();
                }
                last_preview_tick = now_tick;
            }
            if(shown_process_us > 999999u) shown_process_us = 999999u;
            if(shown_interval_us > 999999u) shown_interval_us = 999999u;

            if(status_page == 0)             // 第 1 页: 视觉 / 巡线
            {
                ips200_show_int(150, 160, (int32)final_mid_line, 3);
                ips200_show_int(205, 160, (int32)img_threshold, 3);
                ips200_show_int(45, 185, camera_init_state, 1);
                ips200_show_int(120, 185, (int32)process_count, 5);
                ips200_show_int(60, 205, start, 2);
                ips200_show_int(145, 205, base_speed, 4);
                ips200_show_int(35, 225, motor_duty_L, 5);
                ips200_show_int(145, 225, motor_duty_R, 5);
                ips200_show_int(64, 245, vision_camera_fps, 3);
                ips200_show_int(160, 245, vision_process_fps, 3);
                ips200_show_int(32, 265, (int32)shown_process_us, 6);
                ips200_show_int(144, 265, (int32)shown_interval_us, 6);
                ips200_show_int(40, 285, (int32)vision_copy_reject_count, 5);
                ips200_show_int(144, 285, sw_isr_rate, 2);
                ips200_show_int(40, 304, zt_state, 1);
                ips200_show_int(140, 304, cross_state, 1);
            }
            else if(status_page == 1)        // 第 2 页: 编码器 / 闭环
            {
                ips200_show_int(55, 160, enc_cnt_L, 6);
                ips200_show_int(125, 160, enc_cnt_R, 6);
                ips200_show_int(55, 185, speed_tgt_L, 6);
                ips200_show_int(125, 185, speed_tgt_R, 6);
                ips200_show_int(60, 205, motor_duty_L, 6);
                ips200_show_int(130, 205, motor_duty_R, 6);
                ips200_show_int(35, 225, enc_k_L, 5);
                ips200_show_int(92, 225, enc_k_R, 5);
                ips200_show_int(190, 225, speed_loop_on, 1);
                ips200_show_int(55, 245, speed_loop_fault, 1);
                ips200_show_int(125, 245, base_speed, 4);
                ips200_show_int(190, 245, (int32)control_bend_dbg, 3);
            }
        }

        // 调参菜单: 只在参数/选中项变化时重绘(SPI 写屏会拖慢视觉)
        if(status_page == 2 && tune_menu_version != tune_menu_last_version)
        {
            tune_menu_last_version = tune_menu_version;
            tune_menu_draw();
        }
    }
}

#pragma section all restore
