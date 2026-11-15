/*
 * display.h
 *  IPS200 屏幕: 边线/中线绘制 + 第 3 页调参菜单
 */

#ifndef CODE_DISPLAY_H_
#define CODE_DISPLAY_H_

#include "zf_common_headfile.h"

// 屏幕显示相关宏定义
#define RGB565_BLUE    0x001F  // RGB565 蓝色
#define RGB565_GREEN   0x07E0  // RGB565 绿色
#define RGB565_RED     0xF800  // RGB565 红色
#define DISPLAY_SCALE  1.28f   // 188x120 图像 -> 屏幕的缩放比例

// 赛道边线与中线显示函数
void draw_line(void);

//====================== 屏幕调参菜单(第 3 页) ======================
// 长按 KEY_1 >= 1s 进入/退出; 菜单内 KEY_1/KEY_2 上一项/下一项, KEY_3/KEY_4 减/加。
#define TUNE_ROWS           5       // 一屏显示几项(共 27 项, 自动翻页)
#define TUNE_REPEAT_TICKS   20      // 长按连加/连减的步进间隔, 20 * 5ms = 100ms

extern volatile uint8 tune_menu_active;     // 1 = 菜单占用按键, 车辆强制停止
extern volatile uint8 tune_menu_version;    // 每次改动 +1, 主循环据此重绘
uint8 tune_menu_key_scan(void);             // 5ms 中断里调; 返回 1 表示菜单接管本拍按键
void  tune_menu_draw(void);                 // 绘制菜单页(第 3 页)

#endif /* CODE_DISPLAY_H_ */
