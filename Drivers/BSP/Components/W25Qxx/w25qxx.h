#ifndef DRIVERS_BSP_W25QXX_W25QXX_H
#define DRIVERS_BSP_W25QXX_W25QXX_H

#include "main.h"
#include "spi.h"
#include <stdbool.h>
#include <string.h>

#define W25QXX_CS                      BITBAND_PERIPH(&(W25QXX_CS_GPIO_Port->ODR), 1)

#define W25Qx_PAGE_SIZE                (0x100)
#define W25Qx_SECTOR_SIZE              (W25Qx_PAGE_SIZE * 0x10)
#define W25Qx_BLOCK_SIZE               (W25Qx_SECTOR_SIZE * 0x10)
#define W25Qx_FLASH_SIZE               (W25Qx_BLOCK_SIZE * 0x80)

#define W25Qx_DUMMY_CYCLES_READ        4
#define W25Qx_DUMMY_CYCLES_READ_QUAD   10

#define W25Qx_BULK_ERASE_MAX_TIME      250000
#define W25Qx_SECTOR_ERASE_MAX_TIME    3000
#define W25Qx_SUBSECTOR_ERASE_MAX_TIME 800
#define W25Qx_TIMEOUT_VALUE            1000

/* Reset Operations */
#define RESET_ENABLE_CMD   0x66
#define RESET_MEMORY_CMD   0x99

#define ENTER_QPI_MODE_CMD 0x38
#define EXIT_QPI_MODE_CMD  0xFF

/* Identification Operations */
#define READ_ID_CMD       0x90
#define DUAL_READ_ID_CMD  0x92
#define QUAD_READ_ID_CMD  0x94
#define READ_JEDEC_ID_CMD 0x9F

/* Read Operations */
#define READ_CMD                 0x03
#define FAST_READ_CMD            0x0B
#define DUAL_OUT_FAST_READ_CMD   0x3B
#define DUAL_INOUT_FAST_READ_CMD 0xBB
#define QUAD_OUT_FAST_READ_CMD   0x6B
#define QUAD_INOUT_FAST_READ_CMD 0xEB

/* Write Operations */
#define WRITE_ENABLE_CMD  0x06
#define WRITE_DISABLE_CMD 0x04

/* Register Operations */
#define READ_STATUS_REG1_CMD  0x05
#define READ_STATUS_REG2_CMD  0x35
#define READ_STATUS_REG3_CMD  0x15

#define WRITE_STATUS_REG1_CMD 0x01
#define WRITE_STATUS_REG2_CMD 0x31
#define WRITE_STATUS_REG3_CMD 0x11

/* Program Operations */
#define PAGE_PROG_CMD            0x02
#define QUAD_INPUT_PAGE_PROG_CMD 0x32

/* Erase Operations */
#define SECTOR_ERASE_CMD       0x20
#define CHIP_ERASE_CMD         0xC7

#define PROG_ERASE_RESUME_CMD  0x7A
#define PROG_ERASE_SUSPEND_CMD 0x75

/* Flag Status Register */
#define W25Q64_FSR_BUSY ((uint8_t)0x01) /*!< busy */
#define W25Q64_FSR_WREN ((uint8_t)0x02) /*!< write enable */
#define W25Q64_FSR_QE   ((uint8_t)0x02) /*!< quad enable */

// #define W25Qx_Enable()  HAL_GPIO_WritePin(W25QXX_CS_GPIO_Port, W25QXX_CS_Pin, GPIO_PIN_RESET)
// #define W25Qx_Disable() HAL_GPIO_WritePin(W25QXX_CS_GPIO_Port, W25QXX_CS_Pin, GPIO_PIN_SET)

#define W25Qx_OK      ((uint8_t)0x00)
#define W25Qx_ERROR   ((uint8_t)0x01)
#define W25Qx_BUSY    ((uint8_t)0x02)
#define W25Qx_TIMEOUT ((uint8_t)0x03)

typedef struct w25qxx_struct {
    uint16_t device_id;
    SPI_HandleTypeDef *spi_port;
    bool tx_cplt;
    bool rx_cplt;
} W25QXX_HandleTypeDef;

extern W25QXX_HandleTypeDef hw25q64;

uint8_t BSP_W25Qx_Init(W25QXX_HandleTypeDef *w25qxx, SPI_HandleTypeDef *hspi);
void BSP_W25Qx_Read_ID(W25QXX_HandleTypeDef *w25qxx);
uint8_t BSP_W25Qx_Read(W25QXX_HandleTypeDef *w25qxx, uint8_t *pData, uint32_t ReadAddr, uint32_t Size);
uint8_t BSP_W25Qx_ReadDMA(W25QXX_HandleTypeDef *w25qxx, uint8_t *pData, uint32_t ReadAddr, uint32_t Size);
uint8_t BSP_W25Qx_WriteEnable(W25QXX_HandleTypeDef *w25qxx);
void BSP_W25Qx_WritePage(W25QXX_HandleTypeDef *w25qxx, uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite);
void BSP_W25Qx_EraseWrite(W25QXX_HandleTypeDef *w25qxx, uint8_t *pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite);
uint8_t BSP_W25Qx_EraseSector(W25QXX_HandleTypeDef *w25qxx, uint32_t EraseAddr);
uint8_t BSP_W25Qx_EraseChip(W25QXX_HandleTypeDef *w25qxx);

#endif // DRIVERS_BSP_W25QXX_W25QXX_H