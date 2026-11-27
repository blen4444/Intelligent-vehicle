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
* 本开源库的发布是希望它能发挥作用，但对其不作任何的保证
* 甚至没有隐含的适销性或适合特定用途的保证
* 更多细节请参见 GPL
*
* 您应该在收到本开源库的同时收到一份 GPL 的副本
* 如果没有，请参阅<https://www.gnu.org/licenses/>。
*
* 额外注明：
* 本开源库使用 GPL3.0 开源许可证协议 以上许可申明为译文版本
* 许可申明英文版在 libraries/doc 文件夹下的 GPL3_permission_statement.txt 文件中
* 许可证副本在 libraries 文件夹下 即该文件夹下的 LICENSE 文件
* 欢迎各位使用并传播本程序 但修改内容时必须保留逐飞科技的版权声明（即本声明）
*
* 文件名称          isr
* 公司名称          成都逐飞科技有限公司
* 版本信息          查看 libraries/doc 文件夹内 version 文件 版本说明
* 开发环境          ADS v1.9.20
* 适用平台          TC264D
* 店铺链接          https://seekfree.taobao.com/
*
* 修改记录
* 日期              作者                备注
* 2022-09-15       pudding            first version
********************************************************************************************************************/

#include "isr_config.h"
#include "isr.h"
#include "vision_runtime.h"

// 对于TC系列默认是不支持中断嵌套的，希望支持中断嵌套需要在中断内使用 interrupt_global_enable(0); 来开启中断嵌套
// 简单点说实际上进入中断后TC系列的硬件自动调用了 interrupt_global_disable(); 来拒绝响应任何的中断，因此需要我们自己手动调用 interrupt_global_enable(0); 来开启中断的响应。

// **************************** PIT中断函数 ****************************
// 5ms 控制中断: 按键 + 舵机开环 + 电机差速
IFX_INTERRUPT(cc60_pit_ch0_isr, 0, CCU6_0_CH0_ISR_PRIORITY)
{
    uint8 vision_ok;

    interrupt_global_enable(0);                     // 开启中断嵌套
    pit_clear_flag(CCU60_CH0);
    sys_tick++;

    // 编码器采样(必须早于 control_drive_update), 5ms 一次
    Encoder_Data_Get();
    enc_cnt_L = motor_l.encoder_speed;
    enc_cnt_R = motor_r.encoder_speed;

    // 每5ms统计一次软件PWM中断次数: 正常应为 50 (100us一步)
    {
        static uint32 last_sw_cnt = 0;
        uint32 now_sw_cnt = sw_isr_cnt;
        sw_isr_rate = (uint16)(now_sw_cnt - last_sw_cnt);
        last_sw_cnt = now_sw_cnt;
    }

    key_scanner();

    // 屏幕调参菜单: 长按 KEY_1 >= 1s 进入/退出。菜单占用按键时强制停车, 只走菜单逻辑
    if(tune_menu_key_scan())
    {
        start       = 0;                    // 调参时车必须停住
        status_page = 2;                    // 固定显示第 3 页(菜单)
    }
    else
    {
        if(key_get_state(KEY_1) == KEY_SHORT_PRESS) start ^= 1;          // KEY_1: 启动 / 停止
        if(key_get_state(KEY_2) == KEY_SHORT_PRESS)                      // KEY_2: 切换速度档位
        {
            speed_level++;
            if(speed_level >= SPEED_LEVEL_NUM) speed_level = 0;
            base_speed = speed_table[speed_level];
        }
        if(key_get_state(KEY_4) == KEY_SHORT_PRESS) status_page = (uint8)((status_page == 0) ? 1 : 0);    // KEY_4 短按: 切显示页
        {
            static uint8 key4_long = 0;
            uint8 key4_now = (uint8)(key_get_state(KEY_4) == KEY_LONG_PRESS);
            if(key4_now && !key4_long)                                   // KEY_4 长按: 开环/闭环
            {
                speed_loop_on ^= 1;
                speed_loop_fault = 0;
            }
            key4_long = key4_now;
        }
        if(key_get_state(KEY_3) == KEY_SHORT_PRESS) motor_pol ^= 1;      // KEY_3: 正转 / 倒车
    }

    // 0.5 秒内收到过新图像才认为视觉有效
    vision_ok = ((sys_tick - last_frame_tick) < VISION_TIMEOUT);

    // 舵机开环: 直接按中线偏差打角; 丢线时回中
    control_drive_update((uint8)(start % 2 == 1), vision_ok);

    // 占空比 -> 50 档软件 PWM。用余数累加做抖动:
    // 每拍取整丢掉的零头留到下一拍, 平均下来分辨率优于 1 档(2%),
    // 否则闭环在 4% 这种低占空比下根本没法做细调。
    {
        static int frac_l = 0, frac_r = 0;
        int step_l, step_r;
        frac_l += motor_duty_L * SW_PWM_STEPS;
        step_l  = frac_l / 10000;
        frac_l -= step_l * 10000;
        frac_r += motor_duty_R * SW_PWM_STEPS;
        step_r  = frac_r / 10000;
        frac_r -= step_r * 10000;
        if(step_l > SW_PWM_STEPS) step_l = SW_PWM_STEPS;
        if(step_r > SW_PWM_STEPS) step_r = SW_PWM_STEPS;
        sw_duty_L = (uint8)step_l;
        sw_duty_R = (uint8)step_r;
    }

    // 方向线: P21_2 / P21_4 (1=正转 0=倒车)
    gpio_set_level(MotorL_dir_pin, motor_pol);
    gpio_set_level(MotorR_dir_pin, motor_pol);
}

// 软件PWM: 100us 一步, 50 步 = 200Hz, 输出到 P21_3 / P21_5 (速度线)
IFX_INTERRUPT(cc61_pit_ch0_isr, 0, CCU6_1_CH0_ISR_PRIORITY)
{
    interrupt_global_enable(0);                     // 允许中断嵌套

    pit_clear_flag(CCU61_CH0);

    sw_isr_cnt++;
    sw_phase++;
    if(sw_phase >= SW_PWM_STEPS) sw_phase = 0;

    // 速度线高电平有效: 1 = 驱动, 0 = 停止; 占空比 0 时一直为 0 -> 电机停
    gpio_set_level(MotorL_speed_pin, (sw_phase < sw_duty_L) ? 1 : 0);
    gpio_set_level(MotorR_speed_pin, (sw_phase < sw_duty_R) ? 1 : 0);
}
// **************************** PIT中断函数 ****************************


// **************************** 外部中断函数 ****************************
// ERU CH2 被摄像头 PCLK 占用(触发 DMA), 所以没有 CH2/CH6 的中断函数
IFX_INTERRUPT(exti_ch3_ch7_isr, 0, EXTI_CH3_CH7_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    if(exti_flag_get(ERU_CH3_REQ6_P02_0))           // 通道3中断: 摄像头 VSYNC
    {
        exti_flag_clear(ERU_CH3_REQ6_P02_0);
        // 源缓冲只在主循环 memcpy 期间归主循环所有: 此时跳过这次采集, 不覆盖,
        // PWM 中断照常运行; 否则重新起一次 DMA。
        if(camera_copy_lock)
        {
            vision_skipped_capture_count++;
        }
        else
        {
            camera_capture_epoch++;
            mt9v03x_finish_flag = 0;
            camera_vsync_handler();
        }
    }
}
// **************************** 外部中断函数 ****************************


// **************************** DMA中断函数 ****************************
IFX_INTERRUPT(dma_ch5_isr, 0, DMA_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    camera_dma_handler();                           // 摄像头采集完成统一回调函数
    // 库只在完整有效帧之后置位该标志
    if(mt9v03x_finish_flag) camera_frame_count++;
}
// **************************** DMA中断函数 ****************************


// **************************** 串口中断函数 ****************************
// 串口0默认作为调试串口(发送中断默认关闭, 故不写 TX 中断函数)
IFX_INTERRUPT(uart0_rx_isr, 0, UART0_RX_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套

#if DEBUG_UART_USE_INTERRUPT                        // 如果开启 debug 串口中断
        debug_interrupr_handler();                  // 调用 debug 串口接收处理函数 数据会被 debug 环形缓冲区读取
#endif                                              // 如果修改了 DEBUG_UART_INDEX 那这段代码需要放到对应的串口中断去
}


// 串口1默认连接到摄像头配置串口
IFX_INTERRUPT(uart1_rx_isr, 0, UART1_RX_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    camera_uart_handler();                          // 摄像头参数配置统一回调函数
}

// 串口2默认连接到无线转串口模块
IFX_INTERRUPT(uart2_rx_isr, 0, UART2_RX_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    wireless_module_uart_handler();                 // 无线模块统一回调函数
}
// 串口3默认连接到GPS定位模块
IFX_INTERRUPT(uart3_rx_isr, 0, UART3_RX_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    gnss_uart_callback();                           // GNSS串口回调函数
}

// 串口通讯错误中断
IFX_INTERRUPT(uart0_er_isr, 0, UART0_ER_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    IfxAsclin_Asc_isrError(&uart0_handle);
}
IFX_INTERRUPT(uart1_er_isr, 0, UART1_ER_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    IfxAsclin_Asc_isrError(&uart1_handle);
}
IFX_INTERRUPT(uart2_er_isr, 0, UART2_ER_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    IfxAsclin_Asc_isrError(&uart2_handle);
}
IFX_INTERRUPT(uart3_er_isr, 0, UART3_ER_INT_PRIO)
{
    interrupt_global_enable(0);                     // 开启中断嵌套
    IfxAsclin_Asc_isrError(&uart3_handle);
}
// **************************** 串口中断函数 ****************************
