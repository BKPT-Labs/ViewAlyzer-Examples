#pragma once
/* Device access for the application and for the ViewAlyzer recorder (the
 * recorder is built with VA_DEVICE_HEADER=stm32u3xx.h and does not need this
 * file; it is here for the application sources). */
#include "stm32u3xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

void Error_Handler(void);
