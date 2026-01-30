#ifndef BSP_KEY_KEY_H
#define BSP_KEY_KEY_H

#include "main.h"
#include <stdbool.h>

// typedef enum {
//     key_none = 0,
//     key_s1   = 1,
//     key_s2   = 2,
//     key_s3   = 3,
// } KeyStatus;

extern bool key_reflash_flag;
// extern KeyStatus key_flag;

void handle_key(void);

#endif // !BSP_KEY_KEY_H
