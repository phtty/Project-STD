#pragma once

#include <stdint.h>
#include <stdbool.h>

/*  是否启用UTF8、GBK、UNICODE编码之间的转换
    注意：unicode编码表占用大，若资源受限，建议优化ff_convert函数，将编码表放到SD卡或者FLASH中
*/
#define TEXTCODEC_ENABLE 1

uint8_t chr2hex(uint8_t chr);
uint8_t hex2chr(uint8_t hex);
void HexToStr(const uint8_t *from, uint32_t fromSize, char *to, uint32_t *toSize);
void StrToHex(const char *from, uint32_t fromSize, uint8_t *to, uint32_t *toSize);

#if (TEXTCODEC_ENABLE == 1)

void GBKToUTF8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UTF8ToGBK(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void GBKToUnicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UnicodeToGBK(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UTF8ToUnicode(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);
void UnicodeToUTF8(const char *from, uint32_t fromSize, char *to, uint32_t *toSize);

#endif
