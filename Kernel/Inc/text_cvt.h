#pragma once

/**
 * @file    text_cvt.h
 * @brief   文本编码转换：十六进制与 GBK/UTF-8/Unicode 互转
 */

#include <stdint.h>

/*  是否启用UTF8、GBK、UNICODE编码之间的转换
    注意：unicode编码表占用大，若资源受限，建议优化ff_convert函数，将编码表放到SD卡或者FLASH中
*/
#define CVT_TEXTCODEC_ENABLE 1    /**< 是否启用 GBK/UTF-8/Unicode 之间的转换 */

/** @brief 十六进制字符转数值，如 'A' → 0xA
 *  @param chr 待转换的十六进制字符
 *  @return 对应数值（0..15）；非法字符返回 0 */
uint8_t cvt_chr_to_hex(uint8_t chr);

/** @brief 数值转十六进制字符，如 0xA → 'A'
 *  @param hex 待转换的数值（0..15）
 *  @return 对应的十六进制字符；越界返回 '0' */
uint8_t cvt_hex_to_chr(uint8_t hex);

/** @brief 十六进制数据转字符串，如 {0xAA,0xBB} → "AABB"
 *  @param from 待转换的十六进制数据
 *  @param fromSize 待转换数据字节数
 *  @param[out] to 接收字符串的缓冲；本函数写入 2×fromSize 个字符并以 '\0' 结尾
 *  @param[out] toSize 输出：写入的字符数（不含结束符） */
void cvt_hex_to_str(const uint8_t *from, uint32_t fromSize, char *to, uint32_t *toSize);

/** @brief 十六进制字符串转数据，如 "AABB" → {0xAA,0xBB}
 *  @param from 待转换的十六进制字符串
 *  @param fromSize 字符串长度（字节）
 *  @param[out] to 接收数据的缓冲
 *  @param[out] toSize 输出：写入的字节数 */
void cvt_str_to_hex(const char *from, uint32_t fromSize, uint8_t *to, uint32_t *toSize);

#if (CVT_TEXTCODEC_ENABLE == 1)

/** @brief GBK 码转 UTF-8 码
 *  @param from GBK 码
 *  @param fromSize GBK 码字节数
 *  @param[out] to 接收 UTF-8 码的缓冲
 *  @param[out] toSize 输出：写入的字节数 */
void cvt_gbk_to_utf8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

/** @brief UTF-8 码转 GBK 码
 *  @param from UTF-8 码
 *  @param fromSize UTF-8 码字节数
 *  @param[out] to 接收 GBK 码的缓冲
 *  @param[out] toSize 输出：写入的字节数 */
void cvt_utf8_to_gbk(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

/** @brief GBK 码转双字节 UNICODE 码（小端存储）
 *  @param from GBK 码
 *  @param fromSize GBK 码字节数
 *  @param[out] to 接收 UNICODE 码的缓冲
 *  @param[out] toSize 输出：写入的字节数 */
void cvt_gbk_to_unicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

/** @brief 双字节 UNICODE 码（小端）转 GBK 码
 *  @param from 双字节 UNICODE 码
 *  @param fromSize UNICODE 码字节数
 *  @param[out] to 接收 GBK 码的缓冲
 *  @param[out] toSize 输出：写入的字节数 */
void cvt_unicode_to_gbk(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

/** @brief UTF-8 码转双字节 UNICODE 码（小端存储）
 *  @param from UTF-8 码
 *  @param fromSize UTF-8 码字节数
 *  @param[out] to 接收 UNICODE 码的缓冲
 *  @param[out] toSize 输出：写入的字节数 */
void cvt_utf8_to_unicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

/** @brief 双字节 UNICODE 码（小端）转 UTF-8 码
 *  @param from 双字节 UNICODE 码
 *  @param fromSize UNICODE 码字节数
 *  @param[out] to 接收 UTF-8 码的缓冲
 *  @param[out] toSize 输出：写入的字节数 */
void cvt_unicode_to_utf8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

#endif
