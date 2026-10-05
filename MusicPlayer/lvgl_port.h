#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 QSPI LCD(AXS15231B) + 电容触摸 + LVGL，并创建播放器界面
 * @note  需在 i2c_master_Init() 之后调用
 */
void lvgl_port_init(void);

/**
 * @brief 获取 LVGL 互斥锁（其它任务要操作控件时使用）
 * @param timeout_ms -1 表示永久等待
 */
bool lvgl_port_lock(int timeout_ms);

/** @brief 释放 LVGL 互斥锁 */
void lvgl_port_unlock(void);

/** @brief 诊断用：打印 TCA9554 背光/复位/功放使能引脚的实际电平与 GPIO42 电平 */
void lvgl_port_dump_exio(const char *where);

/* ------------------- 横屏朝向（运行时切换） -------------------
 * 0 = 官方默认（手持设备逆时针转 90°）
 * 1 = 反向（手持设备顺时针转 90°）
 * 切换后立刻生效，不需要重启；HTTP: POST /api/rot?dir=0|1
 * ------------------------------------------------------------ */
void lvgl_port_set_rot_dir(int dir);
int  lvgl_port_rot_dir(void);

/** @brief httpd handler: POST /api/rot?dir=0|1 */
esp_err_t lvgl_port_h_rot(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif
