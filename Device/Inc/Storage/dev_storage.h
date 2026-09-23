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

/** @brief 存储设备操作虚表
 *
 * **返回值约定（全表统一）：0 = 成功，负值 = 失败。**
 * 四个操作的返回语义必须一致，调用方一律按 0 判成功。
 *
 * 历史教训：曾出现 read 返回字节数（len）、write 返回 0 的不一致约定，
 * 而调用方按 0 判成功 —— 于是每次读都被判为 IO 错误，配置永远读不回来，
 * 且现场表现为"存住了但重启不生效"，没有任何报错。
 */
typedef struct dev_storage_ops {
    int32_t (*init)(dev_storage_t *dev);
    int32_t (*read)(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len);
    int32_t (*write)(dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len);
    int32_t (*erase)(dev_storage_t *dev, uint32_t addr, uint32_t len);
    uint32_t (*capacity)(dev_storage_t *dev);
} dev_storage_ops_t;

/** @brief 存储设备基类（派生类必须将其放在第一个成员位置） */
typedef struct dev_storage {
    const dev_storage_ops_t *ops;
    uint32_t capacity;
} dev_storage_t;

/* ---- 便捷内联（调用方无需写 dev->ops->read(dev, ...)） ---- */

static inline int32_t dev_storage_init(dev_storage_t *d)
{
    return (d && d->ops && d->ops->init) ? d->ops->init(d) : -1;
}

static inline int32_t dev_storage_read(dev_storage_t *d, uint32_t addr, uint8_t *buf, uint32_t len)
{
    return (d && d->ops && d->ops->read) ? d->ops->read(d, addr, buf, len) : -1;
}

static inline int32_t dev_storage_write(dev_storage_t *d, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    return (d && d->ops && d->ops->write) ? d->ops->write(d, addr, buf, len) : -1;
}

static inline int32_t dev_storage_erase(dev_storage_t *d, uint32_t addr, uint32_t len)
{
    return (d && d->ops && d->ops->erase) ? d->ops->erase(d, addr, len) : -1;
}

static inline uint32_t dev_storage_capacity(dev_storage_t *d)
{
    return (d && d->ops && d->ops->capacity) ? d->ops->capacity(d) : (d ? d->capacity : 0);
}
