
#include "w25qxx.h"

static uint8_t w25qxx_buff[W25Qx_SECTOR_SIZE] = {0};

W25QXX_HandleTypeDef hw25q64 = {0};

static void BSP_W25Qx_Reset(W25QXX_HandleTypeDef *w25qxx);
static uint8_t BSP_W25Qx_GetStatus(W25QXX_HandleTypeDef *w25qxx);
static void BSP_W25Qx_WriteNoCheck(W25QXX_HandleTypeDef *w25qxx, uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite);

/**
 * @brief W25Qxx初始化
 * @retval W25Qxx状态
 */
uint8_t BSP_W25Qx_Init(W25QXX_HandleTypeDef *w25qxx, SPI_HandleTypeDef *hspi)
{
    w25qxx->spi_port = hspi;

    BSP_W25Qx_Reset(w25qxx);
    BSP_W25Qx_Read_ID(w25qxx);
    return BSP_W25Qx_GetStatus(w25qxx);
}

static void BSP_W25Qx_Reset(W25QXX_HandleTypeDef *w25qxx)
{
    uint8_t buf_cnt       = 0;
    uint8_t reset_buff[2] = {0};

    reset_buff[buf_cnt++] = RESET_ENABLE_CMD;
    reset_buff[buf_cnt++] = RESET_MEMORY_CMD;

    W25QXX_CS = 0;

    HAL_SPI_Transmit(w25qxx->spi_port, reset_buff, buf_cnt, W25Qx_TIMEOUT_VALUE);

    W25QXX_CS = 1;
}

/**
 * @brief 获取W25Qxx状态寄存器1读数
 * @retval W25Qxx状态
 */
static uint8_t BSP_W25Qx_GetStatus(W25QXX_HandleTypeDef *w25qxx)
{
    uint8_t status;
    uint8_t buf_cnt        = 0;
    uint8_t status_buff[2] = {0};

    status_buff[buf_cnt++] = READ_STATUS_REG1_CMD;

    W25QXX_CS = 0;

    /* Send the read status command */
    HAL_SPI_Transmit(w25qxx->spi_port, status_buff, buf_cnt, W25Qx_TIMEOUT_VALUE);
    /* Reception of the data */
    HAL_SPI_Receive(w25qxx->spi_port, &status, 1, W25Qx_TIMEOUT_VALUE);

    W25QXX_CS = 1;

    /* Check the value of the register */
    if ((status & W25Q64_FSR_BUSY) != 0) {
        return W25Qx_BUSY;

    } else {
        return W25Qx_OK;
    }
}

/**
 * @brief W25Qxx写使能
 * @retval W25Qxx状态
 */
uint8_t BSP_W25Qx_WriteEnable(W25QXX_HandleTypeDef *w25qxx)
{
    uint32_t tickstart          = HAL_GetTick();
    uint8_t buf_cnt             = 0;
    uint8_t WriteEnable_buff[2] = {0};

    WriteEnable_buff[buf_cnt++] = WRITE_ENABLE_CMD;

    W25QXX_CS = 0;

    HAL_SPI_Transmit(w25qxx->spi_port, WriteEnable_buff, buf_cnt, W25Qx_TIMEOUT_VALUE);

    W25QXX_CS = 1;

    /* Wait the end of Flash writing */
    while (BSP_W25Qx_GetStatus(w25qxx) == W25Qx_BUSY) {
        if ((HAL_GetTick() - tickstart) > W25Qx_TIMEOUT_VALUE) {
            return W25Qx_TIMEOUT;
        }
    }

    return W25Qx_OK;
}

/**
 * @brief 读取W25Qxx唯一ID
 * @param *ID 存放ID的数组
 * @retval NONE
 */
void BSP_W25Qx_Read_ID(W25QXX_HandleTypeDef *w25qxx)
{
    uint8_t device_id[2] = {0};
    uint8_t buf_cnt      = 0;
    uint8_t id_buff[8]   = {0};

    id_buff[buf_cnt++] = READ_ID_CMD;
    id_buff[buf_cnt++] = 0x00;
    id_buff[buf_cnt++] = 0x00;
    id_buff[buf_cnt++] = 0x00;

    W25QXX_CS = 0;

    HAL_SPI_Transmit(w25qxx->spi_port, id_buff, buf_cnt, W25Qx_TIMEOUT_VALUE);
    HAL_SPI_Receive(w25qxx->spi_port, device_id, 2, W25Qx_TIMEOUT_VALUE);
    w25qxx->device_id = (device_id[0] << 8) | device_id[1];

    W25QXX_CS = 1;
}

/**
 * @brief 查询方式读数据（阻塞）
 * @param *pData    读缓存指令
 * @param ReadAddr  flash的地址
 * @param Size      字节大小
 * @retval W25Q64状态
 */
uint8_t BSP_W25Qx_Read(W25QXX_HandleTypeDef *w25qxx, uint8_t *pData, uint32_t ReadAddr, uint32_t Size)
{
    uint8_t cmd_cnt = 0;
    uint8_t cmd[5]  = {0};

    cmd[cmd_cnt++] = READ_CMD;
    if (w25qxx->device_id >= 0xef18)
        cmd[cmd_cnt++] = (uint8_t)(ReadAddr >> 24);
    cmd[cmd_cnt++] = (uint8_t)(ReadAddr >> 16);
    cmd[cmd_cnt++] = (uint8_t)(ReadAddr >> 8);
    cmd[cmd_cnt++] = (uint8_t)ReadAddr;

    W25QXX_CS = 0;

    HAL_SPI_Transmit(w25qxx->spi_port, cmd, cmd_cnt, 10);

    if (HAL_SPI_Receive(w25qxx->spi_port, pData, Size, 10) != HAL_OK) {
        return W25Qx_ERROR;
    }

    W25QXX_CS = 1;

    return W25Qx_OK;
}

/**
 * @brief DMA方式读数据（非阻塞）
 * @param *pData    读缓存指令
 * @param ReadAddr  flash的地址
 * @param Size      字节大小
 * @retval W25Qxx状态
 */
uint8_t BSP_W25Qx_ReadDMA(W25QXX_HandleTypeDef *w25qxx, uint8_t *pData, uint32_t ReadAddr, uint32_t Size)
{
    uint8_t cmd_cnt = 0;
    uint8_t cmd[5]  = {0};

    cmd[cmd_cnt++] = READ_CMD;
    if (w25qxx->device_id >= 0xef18)
        cmd[cmd_cnt++] = (uint8_t)(ReadAddr >> 24);
    cmd[cmd_cnt++] = (uint8_t)(ReadAddr >> 16);
    cmd[cmd_cnt++] = (uint8_t)(ReadAddr >> 8);
    cmd[cmd_cnt++] = (uint8_t)ReadAddr;

    W25QXX_CS = 0;

    HAL_SPI_Transmit(w25qxx->spi_port, cmd, cmd_cnt, 10); // 发送读取命令

    w25qxx->rx_cplt = false;

    if (HAL_SPI_Receive_DMA(w25qxx->spi_port, pData, Size)) {
        return W25Qx_ERROR;
    }

    while (w25qxx->rx_cplt == false);

    W25QXX_CS = 1;

    while (HAL_SPI_GetState(w25qxx->spi_port) == HAL_SPI_STATE_BUSY_RX);

    return W25Qx_OK;
}

/**
 * @brief 页编程
 *
 * @param w25qxx w25qxx句柄
 * @param pBuffer 写入内容
 * @param WriteAddr flash的地址
 * @param NumByteToWrite 字节大小（最大256B）
 *
 * @note NumByteToWrite不应超过该页的剩余字节数
 *
 * @retval none
 */
void BSP_W25Qx_WritePage(W25QXX_HandleTypeDef *w25qxx, uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    uint8_t cmd[5];
    uint8_t cmd_len = 0;

    BSP_W25Qx_WriteEnable(w25qxx); // 发送 0x06

    // 组装指令和地址
    cmd[cmd_len++] = PAGE_PROG_CMD; // 0x02

    // 只有当Flash容量 > 16MB (W25Q128) 且处于3字节模式下才需要特殊处理
    // 此处假设 W25Q256 (ID: 0xEF19) 使用 4-Byte Address Mode
    if (w25qxx->device_id >= 0xef19) {
        cmd[cmd_len++] = (uint8_t)(WriteAddr >> 24);
    }
    cmd[cmd_len++] = (uint8_t)((WriteAddr) >> 16);
    cmd[cmd_len++] = (uint8_t)((WriteAddr) >> 8);
    cmd[cmd_len++] = (uint8_t)(WriteAddr);

    // 确保Flash不忙
    while (BSP_W25Qx_GetStatus(w25qxx) == W25Qx_BUSY);

    W25QXX_CS = 0; // 使能片选

    // 发送命令和地址
    if (HAL_SPI_Transmit(w25qxx->spi_port, cmd, cmd_len, 100) != HAL_OK) {
        W25QXX_CS = 1;
        return; // 发送失败处理
    }

    // DMA发送数据
    w25qxx->tx_cplt = false;
    if (HAL_SPI_Transmit_DMA(w25qxx->spi_port, pBuffer, NumByteToWrite) == HAL_OK) {
        // 等待传输完成标志，加一个超时机制防止死锁，片选在回调中除能
        uint32_t tickstart = HAL_GetTick();
        while (!w25qxx->tx_cplt) {
            if (HAL_GetTick() - tickstart > 20) { // 256字节传输通常<1ms，给20ms超时
                HAL_SPI_Abort(w25qxx->spi_port);
                break;
            }
        }
    }

    // 等待写入结束 (Flash内部编程)
    while (BSP_W25Qx_GetStatus(w25qxx) == W25Qx_BUSY);
}

/**
 * @brief   无检验写数据
 * @param   *pBuffer          写缓存指令
 * @param   WriteAddr         flash的地址
 * @param   NumByteToWrite    字节大小(最大65535)
 * @retval  NONE
 */
static void BSP_W25Qx_WriteNoCheck(W25QXX_HandleTypeDef *w25qxx, uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    uint16_t PageRemain;
    uint16_t NumByteToWriteNow;
    PageRemain        = W25Qx_PAGE_SIZE - WriteAddr % W25Qx_PAGE_SIZE; // 单页剩余的字节数
    NumByteToWriteNow = NumByteToWrite;
    if (NumByteToWrite <= PageRemain)
        PageRemain = NumByteToWriteNow; // 不大于256个字节
    while (1) {
        BSP_W25Qx_WritePage(w25qxx, pBuffer, WriteAddr, PageRemain);

        if (NumByteToWriteNow == PageRemain) { // 写入结束
            break;

        } else { // NumByteToWrite>PageRemain
            pBuffer += PageRemain;
            WriteAddr += PageRemain;

            NumByteToWriteNow -= PageRemain; // 减去已经写入了的字节数
            if (NumByteToWriteNow > W25Qx_PAGE_SIZE)
                PageRemain = W25Qx_PAGE_SIZE; // 一次可以写入256个字节
            else
                PageRemain = NumByteToWriteNow; // 不够256个字节了
        }
    };
}

/**
 * @brief   通用写数据
 * @param   *pData    写缓存指针
 * @param   WriteAddr flash的地址
 * @param   Size      字节大小
 * @note    会自动识别目标地址所在Sector是否写入数据，如果写入了数据，则会自动擦除
 * @retval  NONE
 */
void BSP_W25Qx_EraseWrite(W25QXX_HandleTypeDef *w25qxx, uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite)
{
    uint16_t secpos;
    uint16_t secoff;
    uint16_t secremain;
    uint16_t i;

    memset(w25qxx_buff, 0, W25Qx_SECTOR_SIZE);
    secpos    = WriteAddr / W25Qx_SECTOR_SIZE; // 扇区地址
    secoff    = WriteAddr % W25Qx_SECTOR_SIZE; // 在扇区内的偏移
    secremain = W25Qx_SECTOR_SIZE - secoff;    // 扇区剩余空间大小

    if (NumByteToWrite <= secremain)
        secremain = NumByteToWrite; // 不大于4096个字节

    while (1) {
        BSP_W25Qx_ReadDMA(w25qxx, w25qxx_buff, secpos * W25Qx_SECTOR_SIZE, W25Qx_SECTOR_SIZE); // 读出整个扇区的内容

        for (i = 0; i < secremain; i++) { // 校验是否存有数据
            if (w25qxx_buff[secoff + i] != 0XFF)
                break; // 需要擦除
        }

        if (i < secremain) { // 需要擦除
            BSP_W25Qx_EraseSector(w25qxx, secpos * W25Qx_SECTOR_SIZE);
            for (i = 0; i < secremain; i++) {
                w25qxx_buff[i + secoff] = pBuffer[i];
            }
            BSP_W25Qx_WriteNoCheck(w25qxx, w25qxx_buff, secpos * W25Qx_SECTOR_SIZE, W25Qx_SECTOR_SIZE); // 写入整个扇区

        } else {
            BSP_W25Qx_WriteNoCheck(w25qxx, pBuffer, WriteAddr, secremain); // 写已擦除地址,直接写入扇区剩余区间.
        }

        if (NumByteToWrite == secremain) { // 写入结束
            break;

        } else {                         // 写入未结束
            secpos++;                    // 扇区地址+1
            secoff = 0;                  // 偏移位置归0
            pBuffer += secremain;        // 指针偏移
            WriteAddr += secremain;      // 写地址偏移
            NumByteToWrite -= secremain; // 字节数递减
            if (NumByteToWrite > W25Qx_SECTOR_SIZE)
                secremain = W25Qx_SECTOR_SIZE; // 下一个扇区还是写不完
            else
                secremain = NumByteToWrite; // 下一个扇区可以写完了
        }
    }
}

/**
 * @brief 扇区擦除
 *
 * @param w25qxx w25qxx句柄
 * @param EraseAddr 扇区编号
 * @return uint8_t 操作结果
 */
uint8_t BSP_W25Qx_EraseSector(W25QXX_HandleTypeDef *w25qxx, uint32_t EraseAddr)
{
    uint8_t cmd[5];
    uint8_t cmd_len    = 0;
    uint32_t tickstart = HAL_GetTick();

    BSP_W25Qx_WriteEnable(w25qxx);

    // 组装指令和地址
    cmd[cmd_len++] = SECTOR_ERASE_CMD;

    if (w25qxx->device_id >= 0xef19) {
        cmd[cmd_len++] = (uint8_t)(EraseAddr >> 24);
    }
    cmd[cmd_len++] = (uint8_t)(EraseAddr >> 16);
    cmd[cmd_len++] = (uint8_t)(EraseAddr >> 8);
    cmd[cmd_len++] = (uint8_t)(EraseAddr);

    /*Select the FLASH: Chip Select low */
    W25QXX_CS = 0;
    /* Send the read ID command */
    HAL_SPI_Transmit(&hspi1, cmd, 4, W25Qx_TIMEOUT_VALUE);
    /*Deselect the FLASH: Chip Select high */
    W25QXX_CS = 1;

    /* Wait the end of Flash writing */
    while (BSP_W25Qx_GetStatus(w25qxx) == W25Qx_BUSY) {
        /* Check for the Timeout */
        if ((HAL_GetTick() - tickstart) > W25Qx_SECTOR_ERASE_MAX_TIME) {
            return W25Qx_TIMEOUT;
        }
    }
    return W25Qx_OK;
}

/**
 * @brief   全片擦除
 * @retval  W25Qxx
 */
uint8_t BSP_W25Qx_EraseChip(W25QXX_HandleTypeDef *w25qxx)
{
    uint8_t cmd[5];
    uint32_t tickstart = HAL_GetTick();
    cmd[0]             = CHIP_ERASE_CMD;

    /* Enable write operations */
    BSP_W25Qx_WriteEnable(w25qxx);

    /*Select the FLASH: Chip Select low */
    W25QXX_CS = 0;
    /* Send the read ID command */
    HAL_SPI_Transmit(&hspi1, cmd, 1, W25Qx_TIMEOUT_VALUE);
    /*Deselect the FLASH: Chip Select high */
    W25QXX_CS = 1;

    /* Wait the end of Flash writing */
    while (BSP_W25Qx_GetStatus(w25qxx) == W25Qx_BUSY) {
        /* Check for the Timeout */
        if ((HAL_GetTick() - tickstart) > W25Qx_BULK_ERASE_MAX_TIME) {
            return W25Qx_TIMEOUT;
        }
    }
    return W25Qx_OK;
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hw25q64.spi_port == hspi) {
        hw25q64.tx_cplt = true;
        // W25QXX_CS       = 1;
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hw25q64.spi_port == hspi) {
        hw25q64.rx_cplt = true;
        W25QXX_CS       = 1;
    }
}
