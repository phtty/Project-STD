#pragma once

#include <stdint.h>

/*  是否启用UTF8、GBK、UNICODE编码之间的转换
    注意：unicode编码表占用大，若资源受限，建议优化ff_convert函数，将编码表放到SD卡或者FLASH中
*/
#define TEXTCODEC_ENABLE 1

uint8_t chr2hex(uint8_t chr);
uint8_t hex2chr(uint8_t hex);
void HexToStr(const uint8_t *from, uint32_t fromSize, char *to, uint32_t *toSize);
void StrToHex(const char *from, uint32_t fromSize, uint8_t *to, uint32_t *toSize);

#if (TEXTCODEC_ENABLE == 1)

/* 编码转换函数族约定：
 *   fromSize  入参：输入字节数
 *   toSize    入参：输出缓冲容量（字节）；出参：实际写出长度（字节）
 * 输入/输出均做越界防护：输入不足一个完整码元或输出容量不足时安全截断终止。 */
void GBKToUTF8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UTF8ToGBK(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void GBKToUnicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UnicodeToGBK(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UTF8ToUnicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UnicodeToUTF8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

#endif
