# 问题记录（Problem Log）

> 工程：`E8_08_mt9v03x_uart_seekfree_assistant_demo`（TC264 + MT9V03X 188x120）
> 记录格式：现象 → 原因 → 解决 → 结果。新增问题请追加在表格最后，并补一节详细说明。

## 一、总览

| # | 日期 | 现象 | 原因 | 解决 | 结果 |
|---|---|---|---|---|---|
| 1 | 2026-11-15 | 转弯时内圈比外圈快，车往弯外顶 / 憋停 | 差速符号写反，且实测 `motor_duty_L`(P21_3) 驱动的是右轮 | `motor_duty_L = cmd + diff; motor_duty_R = cmd - diff;` | 已上车验证，转弯正常；`DIFF_K=0` 可整体关闭差速 |
| 2 | 2026-11-15 | 线上偶发噪点，边线突然飞掉 | `edge_left/edge_right` 取窗口内**第一条**跳变，噪声抢走真边 | 改为取窗口内**离预测位置最近**的跳变 | 已修复，线飞明显减少 |
| 3 | 2026-11-15 | 停车看中线左右抖动，跑起来舵机抖 | 每帧中线直接取平均，无时序约束 | `find_mid_line_weight` 加 `mid_slew`（默认 12 像素/帧）限幅，作用在未加环岛偏置的原始中线上 | 已修复，舵机平稳 |
| 4 | 2026-11-15 | 抬高摄像头后底部缺角、画线断开 | 近场跳过 10 行仍不足 | `jidian_search_line`/`search_start_line` 设 110，底部行用最底有效行填平 | 缓解，但引出问题 6 |
| 5 | 2026-11-15 | 直线/弯道被判成环岛，环岛里卡在 zt4 | 判据只有"单侧长丢线" | 增加近端行范围、排除贴底伪影、要求对侧不丢且基本直线 | 已修复（zt4 误判大幅减少） |
| 6 | 2026-11-16 | 底部整行全白时识别彻底失效：车直行冲出弯道、状态机误进十字 | `find_jidian()` 只扫第 109 行一行，全白时无跳变 → 兜底 `1/186`，且没有有效标志 | 自适应起点行 + 跨帧记忆 + `jidian_valid` + 下游行号相对化 | 已改完，**待上车验证**（见第三节） |

## 二、历史问题补充说明

### 1. 差速方向（2026-11-15）
- 约定：`mid` 小 = 目标在左；`err = MID_W - mid > 0` = 弯在左 → 左轮内圈要减速。
- 实测发现 `motor_duty_L`（`sw_duty_L` → P21_3）实际驱动**右轮**，`motor_duty_R`（P21_5）才是左轮。
- 正确写法：`motor_duty_L = cmd + diff; motor_duty_R = cmd - diff;`
- 注意：闭环 `speed_loop_apply()` 是 `motor_duty_L` 配 `enc_cnt_L(TIM5)`，若 P21_3 与 TIM5 不是同一轮，闭环前需交换配对。

### 2. 边线抗噪（2026-11-15）
- 原来：窗口内找到第一条 `0→255` / `255→0` 就返回。
- 现在：遍历窗口，取**离上一行预测位置最近**的那条边；单侧丢线时窗口从 ±`WIN` 扩到 ±(`WIN`+`LOST`) 再找一次。

### 3. 中线时序限幅（2026-11-15）
- `last_mid_line` 保存**未加环岛偏置**的原始中线，用于下一帧限幅参考；限幅后再叠加环岛偏置，避免拖慢进环打角。

## 三、问题 6：底部整行全白导致基点失效（2026-11-16）

### 现象
摄像头角度压低后，图像最底部（第 110~119 行）左右边线已经落在视野之外，整行都是白色赛道。
此时：
1. `find_jidian()` 在 109 行找不到任何 `0→255` / `255→0` 跳变，直接兜底为 `left_jidian=1` / `right_jidian=186`；
2. 追踪窗口只有 ±2 / ±9，若真实边线在 x≥11 处才出现，窗口永远够不到 → **整侧丢线**；
3. 双侧都丢 → `lx/rx` 保持 `1/186` → `mid = (1+186)/2 = 93 = MID_W` → **车直行**，弯道冲出；
4. `left_lost/right_lost` 连续 ≥ `C_LOSS(25)` 行 → `cross_state=1` → 状态机**误进 ZT_CROSS**，环岛判据全部失效；
5. `left/right_line_list` 全为 `EDGE_OFFSCREEN` → 控制层 near/far 段无效 → `geometry_valid=0`，曲率前馈和弯道减速同时失效。

另外发现两个隐患：
- `real_ly/real_ry` 固定初始化为 109，首次切线斜率基线过长；
- `find_jidian()` 没有任何"本帧基点是否可信"的输出，调用方无法区分。

### 解决（本次修改）

| 文件 | 修改 |
|---|---|
| `code/image.h` | 新增 `JIDIAN_SCAN_TOP`(70)；新增 `extern uint8 jidian_valid`、`extern uint8 search_top_row` |
| `code/image.c` | 重写 `find_jidian()`：从 109 行**向上逐行**找第一个"左右都有真实跳变"的行作起点行 |
| `code/image.c` | 新增 `jidian_last_l/r/w/ok` **跨帧记忆**：本帧全都找不到时沿用上一帧有效基点，再不行才退回 `1/186` |
| `code/image.c` | 新增 `jidian_white_mid()`：两边界之间必须 ≥80% 为白，防止噪声点被当成边线 |
| `code/image.c` | `image_deal()` 起点改为 `top_y = search_top_row`，`real_ly/real_ry` 用 `top_y` 初始化；底部填平区改为 `y > top_y` 复制 `top_y` 行 |
| `code/image.c` | 下游全部行号相对化：`collect_lost_runs`、`find_longest_loss`、`track_width_est`、`mid_bend_est`、`ring_candidate`、`zt4→zt5` 贴底判据、`find_mid_line_weight` |
| `code/control.c` | near/far 采样行由写死的 85~105 / 55~75 改为 `top-24~top-4` / `top-54~top-34`（`top=109` 时与原来完全等价） |
| `user/cpu0_main.c` | 状态页第 1 页底部新增 `top:` 显示实际起点行 |

### 判据要点
- 起点行必须满足：左右跳变都存在、宽度 ∈ [20, 170]、两边界之间 ≥80% 白；
- 第一轮（pass 0）额外要求与上一帧宽度差 ≤35，防止抓到阴影/反光；
- 第二轮（pass 1）去掉宽度连续性约束，避免十字/宽窄突变处被上一帧宽度"锁死"；
- 最多向上试到 `JIDIAN_SCAN_TOP = 70`（即最多跳过 40 行）。

### 结果 / 验证方法
- 编译：AURIX Development Studio 打开 `.project`，`Debug/` 输出，无新增告警。
- 静态验证：`search_top_row` 与 `jidian_valid` 已全部贯通，无遗留 `search_start_line` 直接用于循环边界（仅 `ring_candidate` 用它换算 `NEAR_ROW` 的相对距离）。
- 上车验证步骤：
  1. 停车，屏幕第 1 页看 `top:`。正常情况下底部有边线时应显示 `109`；底部全白时应显示 `90~100` 之间的值；
  2. 观察预览图：左右边线应从 `top` 行开始，不再出现"整段丢线但中线恒 93"；
  3. 走一道缓弯：`final_mid_line` 应随弯道变化；若仍恒为 93，说明 `top` 已抬得太高（底部全白行数超过 40），需要把摄像头角度回调或增大 `JIDIAN_SCAN_TOP` 以外的搜索范围；
  4. 环岛：确认 zt4 仍能被触发（`NEAR_ROW` 判据已改为相对 `top` 的距离，默认行为不变）。
- 待观察项：`top` 在颠簸路面是否会跳变（跳变会让 `mid_line_list` 的统计行集合变化，可能带来轻微抖动）。若出现，可在 `find_jidian()` 里对 `search_top_row` 加一阶滤波（`top = (top*3 + new)/4`）。

### 回滚方法
把 `search_top_row` 固定为 `jidian_search_line - 1`（即 109），并把 `find_jidian()` 循环改成只试这一行，即可回到旧行为。

## 四、已知遗留 / 待办
- `Servo.c`、`led.c`、`pit.c`、`Motor.c` 的若干函数未被调用；`led.c` 的 P21_4 与右电机方向脚冲突，勿启用。
- 环岛出环判据（`W_JUMP`）仍依赖赛道宽度突变，十字之后紧跟环岛时可能误判，需实车确认。
- `display.c` 的预览刷新与状态页刷新都在 CPU0，帧率高时注意 SPI 占用。
