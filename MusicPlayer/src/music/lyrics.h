/**
 * @file  lyrics.h
 * @brief LRC 歌词存储与时间轴检索
 *
 * 编码策略：ESP32 侧不处理 GBK。歌词文件由浏览器读取，
 * 浏览器用 TextDecoder 自动识别 UTF-8/GBK 并解析成「规范化 LRC 文本」，
 * 再通过 POST /api/lyrics 推给板子。板子只解析已是 UTF-8 的规范化文本，
 * 并负责按播放位置检索当前行（位置由板子本地插值，锁屏也不掉词）。
 */
#ifndef LYRICS_H
#define LYRICS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LYRIC_MAX_LINES   500     /* 最多歌词行数          */
#define LYRIC_TEXT_LEN    192     /* 单行歌词最大字节数     */
#define LYRIC_META_LEN    128     /* 标题/艺人最大字节数    */

/** @brief 清空当前歌词 */
void lyrics_reset(void);

/**
 * @brief 解析规范化 LRC 文本（UTF-8）
 * @param lrc  文本指针（不要求 \0 结尾）
 * @param len  字节数
 * @return 解析到的歌词行数；-1 表示内存不足
 */
int lyrics_parse(const char *lrc, size_t len);

/** @brief 已解析的歌词行数 */
int lyrics_count(void);

/** @brief 第 i 行文本，越界返回 NULL */
const char *lyrics_line(int i);

/** @brief 第 i 行的时间戳（毫秒），越界返回 0 */
uint32_t lyrics_time_ms(int i);

/**
 * @brief 求 pos_ms 时刻应显示的歌词行索引
 * @return 最后一个 time <= pos_ms 的行索引；若 pos 早于首行或没有歌词，返回 -1
 */
int lyrics_index_at(uint32_t pos_ms);

/** @brief 元数据：[ti]标题 / [ar]艺人 / [al]专辑，which = 0/1/2；无则返回 "" */
const char *lyrics_meta(int which);

/** @brief 是否已加载歌词 */
bool lyrics_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* LYRICS_H */
