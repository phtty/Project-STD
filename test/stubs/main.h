/**
 * @file    main.h
 * @brief   host 单测用的 CubeMX main.h 替身
 *
 * app_ldi_cmd.h 包含了 main.h，但实际只用到基础类型。
 * 真 main.h 会拉进 HAL 与 Core 配置，host 上编不了。
 *
 * 这里只提供基础类型：一旦哪天它真开始用 HAL 类型，host 编译会立刻失败，
 * 而不会静默漂移 —— 这正是替身该有的失败方式。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
