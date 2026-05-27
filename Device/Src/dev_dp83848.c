#include "dev_dp83848.h"

#define DP83848_MAX_DEV_ADDR ((uint32_t)31U)

/**
 * @brief  向 PHY 设备对象注册 IO 操作函数
 * @param  obj    DP83848 设备对象
 * @param  io_ctx IO 操作函数集（读/写寄存器、获取时间戳等）
 * @retval DP83848_STATUS_OK    注册成功
 * @retval DP83848_STATUS_ERROR 缺失必要的 IO 函数
 */
int32_t dev_dp83848_register_bus_io(dev_dp83848_obj_t *obj, dev_dp83848_io_ctx_t *io_ctx)
{
    if (!obj || !io_ctx->read_reg || !io_ctx->write_reg || !io_ctx->get_tick) {
        return DP83848_STATUS_ERROR;
    }

    obj->io.init     = io_ctx->init;
    obj->io.deinit   = io_ctx->deinit;
    obj->io.read_reg  = io_ctx->read_reg;
    obj->io.write_reg = io_ctx->write_reg;
    obj->io.get_tick  = io_ctx->get_tick;

    return DP83848_STATUS_OK;
}

/**
 * @brief  初始化 DP83848 PHY 并扫描设备地址
 * @param  obj  DP83848 设备对象
 * @retval DP83848_STATUS_OK            初始化成功
 * @retval DP83848_STATUS_ADDRESS_ERROR 未找到 PHY 设备地址
 * @retval DP83848_STATUS_READ_ERROR    寄存器读取失败
 */
int32_t dev_dp83848_init(dev_dp83848_obj_t *obj)
{
    uint32_t regvalue = 0, addr = 0;
    int32_t status = DP83848_STATUS_OK;

    if (obj->is_initialized == 0) {
        if (obj->io.init != 0) {
            /* GPIO 和时钟初始化 */
            obj->io.init();
        }

        /* 预设一个无效地址，供后续校验 */
        obj->dev_addr = DP83848_MAX_DEV_ADDR + 1;

        /* 遍历地址空间，从 SMR 寄存器读取 PHY 地址 */
        for (addr = 0; addr <= DP83848_MAX_DEV_ADDR; addr++) {
            if (obj->io.read_reg(addr, DP83848_SMR, &regvalue) < 0) {
                status = DP83848_STATUS_READ_ERROR;
                /* 此地址无法读取，继续尝试下一地址 */
                continue;
            }

            if ((regvalue & DP83848_SMR_PHY_ADDR) == addr) {
                obj->dev_addr = addr;
                status        = DP83848_STATUS_OK;
                break;
            }
        }

        if (obj->dev_addr > DP83848_MAX_DEV_ADDR) {
            status = DP83848_STATUS_ADDRESS_ERROR;
        }

        /* 地址匹配成功则标记初始化完成 */
        if (status == DP83848_STATUS_OK) {
            obj->is_initialized = 1;
        }
    }

    return status;
}

/**
 * @brief  反初始化 DP83848，释放硬件资源
 * @param  obj  DP83848 设备对象
 * @retval DP83848_STATUS_OK    成功
 * @retval DP83848_STATUS_ERROR 反初始化失败
 */
int32_t dev_dp83848_deinit(dev_dp83848_obj_t *obj)
{
    if (obj->is_initialized) {
        if (obj->io.deinit != 0) {
            if (obj->io.deinit() < 0) {
                return DP83848_STATUS_ERROR;
            }
        }

        obj->is_initialized = 0;
    }

    return DP83848_STATUS_OK;
}

/**
 * @brief  退出 PHY 省电模式
 * @param  obj  DP83848 设备对象
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_power_down_disable(dev_dp83848_obj_t *obj)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_BCR, &readval) >= 0) {
        readval &= ~DP83848_BCR_POWER_DOWN;

        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_BCR, readval) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  进入 PHY 省电模式
 * @param  obj  DP83848 设备对象
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_power_down_enable(dev_dp83848_obj_t *obj)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_BCR, &readval) >= 0) {
        readval |= DP83848_BCR_POWER_DOWN;

        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_BCR, readval) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  启动自动协商
 * @param  obj  DP83848 设备对象
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_autonego_start(dev_dp83848_obj_t *obj)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_BCR, &readval) >= 0) {
        readval |= DP83848_BCR_AUTONEGO_EN;

        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_BCR, readval) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  获取 DP83848 当前链路状态（速率、双工模式）
 * @param  obj   DP83848 设备对象
 * @retval DP83848_STATUS_LINK_DOWN            链路断开
 * @retval DP83848_STATUS_AUTONEGO_NOTDONE     自动协商未完成
 * @retval DP83848_STATUS_100MBITS_FULLDUPLEX  100M 全双工
 * @retval DP83848_STATUS_100MBITS_HALFDUPLEX  100M 半双工
 * @retval DP83848_STATUS_10MBITS_FULLDUPLEX   10M 全双工
 * @retval DP83848_STATUS_10MBITS_HALFDUPLEX   10M 半双工
 * @retval DP83848_STATUS_READ_ERROR           寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR          寄存器写入失败
 */
int32_t dev_dp83848_link_state_get(dev_dp83848_obj_t *obj)
{
    uint32_t readval = 0;

    /* 读取 BSR 状态寄存器（读两次确保结果稳定） */
    if (obj->io.read_reg(obj->dev_addr, DP83848_BSR, &readval) < 0) {
        return DP83848_STATUS_READ_ERROR;
    }
    if (obj->io.read_reg(obj->dev_addr, DP83848_BSR, &readval) < 0) {
        return DP83848_STATUS_READ_ERROR;
    }

    if ((readval & DP83848_BSR_LINK_STATUS) == 0) {
        /* 链路断开 */
        return DP83848_STATUS_LINK_DOWN;
    }

    /* 检查自动协商状态 */
    if (obj->io.read_reg(obj->dev_addr, DP83848_BCR, &readval) < 0) {
        return DP83848_STATUS_READ_ERROR;
    }

    if ((readval & DP83848_BCR_AUTONEGO_EN) != DP83848_BCR_AUTONEGO_EN) {
        /* 自动协商未使能，直接从 BCR 读取速率和双工配置 */
        if (((readval & DP83848_BCR_SPEED_SELECT) == DP83848_BCR_SPEED_SELECT) && ((readval & DP83848_BCR_DUPLEX_MODE) == DP83848_BCR_DUPLEX_MODE)) {
            return DP83848_STATUS_100MBITS_FULLDUPLEX;
        } else if ((readval & DP83848_BCR_SPEED_SELECT) == DP83848_BCR_SPEED_SELECT) {
            return DP83848_STATUS_100MBITS_HALFDUPLEX;
        } else if ((readval & DP83848_BCR_DUPLEX_MODE) == DP83848_BCR_DUPLEX_MODE) {
            return DP83848_STATUS_10MBITS_FULLDUPLEX;
        } else {
            return DP83848_STATUS_10MBITS_HALFDUPLEX;
        }
    } else /* 自动协商已使能 */
    {
        if (obj->io.read_reg(obj->dev_addr, DP83848_PHYSCSR, &readval) < 0) {
            return DP83848_STATUS_READ_ERROR;
        }

        /* 检查自动协商是否完成 */
        if ((readval & DP83848_PHYSCSR_AUTONEGO_DONE) == 0) {
            return DP83848_STATUS_AUTONEGO_NOTDONE;
        }

        if ((readval & DP83848_PHYSCSR_HCDSPEEDMASK) == DP83848_PHYSCSR_100BTX_FD) {
            return DP83848_STATUS_100MBITS_FULLDUPLEX;
        } else if ((readval & DP83848_PHYSCSR_HCDSPEEDMASK) == DP83848_PHYSCSR_100BTX_HD) {
            return DP83848_STATUS_100MBITS_HALFDUPLEX;
        } else if ((readval & DP83848_PHYSCSR_HCDSPEEDMASK) == DP83848_PHYSCSR_10BT_FD) {
            return DP83848_STATUS_10MBITS_FULLDUPLEX;
        } else {
            return DP83848_STATUS_10MBITS_HALFDUPLEX;
        }
    }
}

/**
 * @brief  手动设置 DP83848 链路速率和双工模式（关闭自动协商）
 * @param  obj        DP83848 设备对象
 * @param  link_state 目标链路状态：
 *                    DP83848_STATUS_100MBITS_FULLDUPLEX
 *                    DP83848_STATUS_100MBITS_HALFDUPLEX
 *                    DP83848_STATUS_10MBITS_FULLDUPLEX
 *                    DP83848_STATUS_10MBITS_HALFDUPLEX
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_ERROR        无效的链路状态参数
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_link_state_set(dev_dp83848_obj_t *obj, uint32_t link_state)
{
    uint32_t bcrvalue = 0;
    int32_t status    = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_BCR, &bcrvalue) >= 0) {
        /* 关闭自动协商、速率和双工的现有配置 */
        bcrvalue &= ~(DP83848_BCR_AUTONEGO_EN | DP83848_BCR_SPEED_SELECT | DP83848_BCR_DUPLEX_MODE);

        if (link_state == DP83848_STATUS_100MBITS_FULLDUPLEX) {
            bcrvalue |= (DP83848_BCR_SPEED_SELECT | DP83848_BCR_DUPLEX_MODE);
        } else if (link_state == DP83848_STATUS_100MBITS_HALFDUPLEX) {
            bcrvalue |= DP83848_BCR_SPEED_SELECT;
        } else if (link_state == DP83848_STATUS_10MBITS_FULLDUPLEX) {
            bcrvalue |= DP83848_BCR_DUPLEX_MODE;
        } else {
            /* 无效的链路状态参数 */
            status = DP83848_STATUS_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    if (status == DP83848_STATUS_OK) {
        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_BCR, bcrvalue) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    }

    return status;
}

/**
 * @brief  使能 PHY 环回模式（调试用）
 * @param  obj  DP83848 设备对象
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_loopback_enable(dev_dp83848_obj_t *obj)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_BCR, &readval) >= 0) {
        readval |= DP83848_BCR_LOOPBACK;

        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_BCR, readval) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  关闭 PHY 环回模式
 * @param  obj  DP83848 设备对象
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_loopback_disable(dev_dp83848_obj_t *obj)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_BCR, &readval) >= 0) {
        readval &= ~DP83848_BCR_LOOPBACK;

        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_BCR, readval) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  使能 PHY 中断源
 * @param  obj        DP83848 设备对象
 * @param  interrupt  中断源掩码，可以是以下值的组合：
 *                    DP83848_WOL_IT
 *                    DP83848_ENERGYON_IT
 *                    DP83848_AUTONEGO_COMPLETE_IT
 *                    DP83848_REMOTE_FAULT_IT
 *                    DP83848_LINK_DOWN_IT
 *                    DP83848_AUTONEGO_LP_ACK_IT
 *                    DP83848_PARALLEL_DETECTION_FAULT_IT
 *                    DP83848_AUTONEGO_PAGE_RECEIVED_IT
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_it_enable(dev_dp83848_obj_t *obj, uint32_t interrupt)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_IMR, &readval) >= 0) {
        readval |= interrupt;

        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_IMR, readval) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  关闭 PHY 中断源
 * @param  obj        DP83848 设备对象
 * @param  interrupt  中断源掩码，可以是以下值的组合：
 *                    DP83848_WOL_IT
 *                    DP83848_ENERGYON_IT
 *                    DP83848_AUTONEGO_COMPLETE_IT
 *                    DP83848_REMOTE_FAULT_IT
 *                    DP83848_LINK_DOWN_IT
 *                    DP83848_AUTONEGO_LP_ACK_IT
 *                    DP83848_PARALLEL_DETECTION_FAULT_IT
 *                    DP83848_AUTONEGO_PAGE_RECEIVED_IT
 * @retval DP83848_STATUS_OK           成功
 * @retval DP83848_STATUS_READ_ERROR   寄存器读取失败
 * @retval DP83848_STATUS_WRITE_ERROR  寄存器写入失败
 */
int32_t dev_dp83848_it_disable(dev_dp83848_obj_t *obj, uint32_t interrupt)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_IMR, &readval) >= 0) {
        readval &= ~interrupt;

        /* 写入配置 */
        if (obj->io.write_reg(obj->dev_addr, DP83848_IMR, readval) < 0) {
            status = DP83848_STATUS_WRITE_ERROR;
        }
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  清除 PHY 中断标志（读 ISFR 即清除）
 * @param  obj        DP83848 设备对象
 * @param  interrupt  中断标志掩码（未使用，寄存器的读操作即清除标志）
 * @retval DP83848_STATUS_OK         成功
 * @retval DP83848_STATUS_READ_ERROR 寄存器读取失败
 */
int32_t dev_dp83848_it_clear(dev_dp83848_obj_t *obj, uint32_t interrupt)
{
    uint32_t readval = 0;
    int32_t status   = DP83848_STATUS_OK;

    if (obj->io.read_reg(obj->dev_addr, DP83848_ISFR, &readval) < 0) {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}

/**
 * @brief  获取 PHY 中断标志状态
 * @param  obj        DP83848 设备对象
 * @param  interrupt  待检查的中断标志，可以是以下值的组合：
 *                    DP83848_WOL_IT
 *                    DP83848_ENERGYON_IT
 *                    DP83848_AUTONEGO_COMPLETE_IT
 *                    DP83848_REMOTE_FAULT_IT
 *                    DP83848_LINK_DOWN_IT
 *                    DP83848_AUTONEGO_LP_ACK_IT
 *                    DP83848_PARALLEL_DETECTION_FAULT_IT
 *                    DP83848_AUTONEGO_PAGE_RECEIVED_IT
 * @retval 1  中断标志置位
 * @retval 0  中断标志未置位
 * @retval DP83848_STATUS_READ_ERROR  寄存器读取失败
 */
int32_t dev_dp83848_it_status_get(dev_dp83848_obj_t *obj, uint32_t interrupt)
{
    uint32_t readval = 0;
    int32_t status   = 0;

    if (obj->io.read_reg(obj->dev_addr, DP83848_ISFR, &readval) >= 0) {
        status = ((readval & interrupt) == interrupt);
    } else {
        status = DP83848_STATUS_READ_ERROR;
    }

    return status;
}
