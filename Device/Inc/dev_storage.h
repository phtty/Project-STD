/**
 * @file    dev_storage.h
 * @brief   存储设备公共基类（OCP 虚表模式）
 *
 * 所有存储介质（SPI Flash / 内部 Flash / EEPROM / TF 卡）统一抽象。
 * 派生类将 dev_storage_t 作为第一个成员，通过 container_of 向上/向下转型。
 */

#pragma once

#include <stdint.h>

typedef struct dev_storage dev_storage_t;

/** @brief 存储设备操作虚表 */
typedef struct dev_storage_ops {
    int32_t  (*init)   (dev_storage_t *dev);
    int32_t  (*read)   (dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len);
    int32_t  (*write)  (dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len);
    int32_t  (*erase)  (dev_storage_t *dev, uint32_t addr, uint32_t len);
    uint32_t (*capacity)(dev_storage_t *dev);
} dev_storage_ops_t;

/** @brief 存储设备基类（派生类必须将其放在第一个成员位置） */
typedef struct dev_storage {
    const dev_storage_ops_t *ops;
    uint32_t capacity;
} dev_storage_t;

/* ---- 便捷内联（调用方无需写 dev->ops->read(dev, ...)） ---- */

static inline int32_t dev_storage_init(dev_storage_t *d)
    { return d->ops->init ? d->ops->init(d) : 0; }

static inline int32_t dev_storage_read(dev_storage_t *d, uint32_t addr, uint8_t *buf, uint32_t len)
    { return d->ops->read(d, addr, buf, len); }

static inline int32_t dev_storage_write(dev_storage_t *d, uint32_t addr, const uint8_t *buf, uint32_t len)
    { return d->ops->write(d, addr, buf, len); }

static inline int32_t dev_storage_erase(dev_storage_t *d, uint32_t addr, uint32_t len)
    { return d->ops->erase(d, addr, len); }

static inline uint32_t dev_storage_capacity(dev_storage_t *d)
    { return d->ops->capacity ? d->ops->capacity(d) : d->capacity; }
