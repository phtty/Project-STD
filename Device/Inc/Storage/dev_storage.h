/**
 * @file    dev_storage.h
 * @brief   存储设备公共基类（OCP 虚表模式）
 *
 * 所有存储介质（SPI Flash / 内部 Flash / EEPROM / TF 卡）统一抽象。
 * 派生类将 dev_storage_t 作为第一个成员，通过 container_of 向上/向下转型。
 */

#pragma once

#include <stdint.h>

/** @brief 存储设备（不透明句柄，派生类 embed 之）*/
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
    int32_t (*init)(dev_storage_t *dev); /**< 初始化；写入 dev 的派生字段 */
    int32_t (*read)(dev_storage_t *dev, uint32_t addr, uint8_t *buf, uint32_t len); /**< 读取 len 字节到 buf */
    int32_t (*write)(dev_storage_t *dev, uint32_t addr, const uint8_t *buf, uint32_t len); /**< 写入 len 字节（buf 只读）*/
    int32_t (*erase)(dev_storage_t *dev, uint32_t addr, uint32_t len); /**< 擦除 len 字节 */
    uint32_t (*capacity)(dev_storage_t *dev); /**< 返回容量（字节）*/
} dev_storage_ops_t;

/** @brief 存储设备基类（派生类必须将其放在第一个成员位置） */
typedef struct dev_storage {
    const dev_storage_ops_t *ops; /**< 操作虚表；由派生类型在 init 中绑定 */
    uint32_t capacity;            /**< 容量（字节）；由派生类型实例初始化时填写 */
} dev_storage_t;

/* ---- 便捷内联（调用方无需写 dev->ops->read(dev, ...)） ---- */

/** @brief 初始化存储设备
 *  @param[in,out] d 存储设备；底层 init 会写入其派生字段
 *  @return 0 成功，负值失败 */
static inline int32_t dev_storage_init(dev_storage_t *d)
{
    return (d && d->ops && d->ops->init) ? d->ops->init(d) : -1;
}

/** @brief 从存储读取数据
 *  @param d    存储设备
 *  @param addr 起始地址
 *  @param[out] buf 接收缓冲；本函数写入 len 字节
 *  @param len  读取长度
 *  @return 0 成功，负值失败 */
static inline int32_t dev_storage_read(dev_storage_t *d, uint32_t addr, uint8_t *buf, uint32_t len)
{
    return (d && d->ops && d->ops->read) ? d->ops->read(d, addr, buf, len) : -1;
}

/** @brief 向存储写入数据
 *  @param d    存储设备
 *  @param addr 起始地址
 *  @param buf  待写数据（只读）
 *  @param len  写入长度
 *  @return 0 成功，负值失败 */
static inline int32_t dev_storage_write(dev_storage_t *d, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    return (d && d->ops && d->ops->write) ? d->ops->write(d, addr, buf, len) : -1;
}

/** @brief 擦除一段存储区
 *  @param d    存储设备
 *  @param addr 起始地址
 *  @param len  擦除长度
 *  @return 0 成功，负值失败 */
static inline int32_t dev_storage_erase(dev_storage_t *d, uint32_t addr, uint32_t len)
{
    return (d && d->ops && d->ops->erase) ? d->ops->erase(d, addr, len) : -1;
}

/** @brief 查询存储容量
 *  @param d 存储设备
 *  @return 容量（字节）*/
static inline uint32_t dev_storage_capacity(dev_storage_t *d)
{
    return (d && d->ops && d->ops->capacity) ? d->ops->capacity(d) : (d ? d->capacity : 0);
}
