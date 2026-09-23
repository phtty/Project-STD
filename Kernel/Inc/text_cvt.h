#pragma once

/**
 * @file    text_cvt.h
 * @brief   文本编码转换：十六进制与 GBK/UTF-8/Unicode 互转
 */

#include <stdint.h>

/*  是否启用UTF8、GBK、UNICODE编码之间的转换
    注意：unicode编码表占用大，若资源受限，建议优化ff_convert函数，将编码表放到SD卡或者FLASH中
*/
#define CVT_TEXTCODEC_ENABLE 1

uint8_t cvt_chr_to_hex(uint8_t chr);
uint8_t cvt_hex_to_chr(uint8_t hex);
void cvt_hex_to_str(const uint8_t *from, uint32_t fromSize, char *to, uint32_t *toSize);
void cvt_str_to_hex(const char *from, uint32_t fromSize, uint8_t *to, uint32_t *toSize);

#if (CVT_TEXTCODEC_ENABLE == 1)

void cvt_gbk_to_utf8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void cvt_utf8_to_gbk(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void cvt_gbk_to_unicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void cvt_unicode_to_gbk(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void cvt_utf8_to_unicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void cvt_unicode_to_utf8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

#endif
