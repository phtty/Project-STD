/**
 * @file    dev_flash_font.h
 * @brief   W25Qxx SPI Flash 字库存储设备
 */

#pragma once

#include <stdint.h>

typedef struct {
    void *spi;
    uint32_t capacity;
} font_flash_dev_t;

extern font_flash_dev_t g_font_flash;

void dev_font_flash_init(font_flash_dev_t *dev);
int32_t dev_font_flash_read(font_flash_dev_t *dev, uint32_t addr, uint8_t *buf, uint16_t len);

/* ---- 写 / 擦除 ---- */
int32_t dev_font_flash_write_enable(font_flash_dev_t *dev);
int32_t dev_font_flash_write_page(font_flash_dev_t *dev, uint32_t addr, const uint8_t *buf, uint16_t len);
int32_t dev_font_flash_write(font_flash_dev_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len);
int32_t dev_font_flash_erase_sector(font_flash_dev_t *dev, uint32_t addr);
int32_t dev_font_flash_erase_chip(font_flash_dev_t *dev);
int32_t dev_font_flash_wait_busy(font_flash_dev_t *dev, uint32_t timeout_ms);
