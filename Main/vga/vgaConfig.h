#ifndef VGA_VGACONFIG_H
#define VGA_VGACONFIG_H

#include <string.h>
#include "stm32f4xx_hal.h"

#ifdef BOARD2

// PD0 	Red 1
// PD1	Red 2
// PD2 	Green 1
// PD3 	Green 2
// PD4 	Blue 1
// PD5 	Blue 2
// PB6 	HSync   // TIM4CH1
// PB7 	VSync
#define VIDEO_GPIO_PORT GPIOD
#define VIDEO_GPIO_MASK 0x003Fu
#define VIDEO_GPIO_ODR_BYTE ((uint32_t)((uint8_t *)&GPIOD->ODR + 0))
#define SYNC_GPIO_PORT GPIOB
#define HSYNC_PIN 6u
#define VSYNC_PIN 7u

#define SYNC_GPIO_CLK_ENABLE()  __HAL_RCC_GPIOB_CLK_ENABLE()
#define VIDEO_GPIO_CLK_ENABLE() __HAL_RCC_GPIOD_CLK_ENABLE()

#else

// PE8 		Red 1
// PE9 		Red 2
// PE10 	Green 1
// PE11 	Green 2
// PE12 	Blue 1
// PE13 	Blue 2
// PD15 	HSync   // TIM4CH4
// PD14 	VSync
#define VIDEO_GPIO_PORT GPIOE
#define VIDEO_GPIO_MASK 0x3F00u
#define VIDEO_GPIO_ODR_BYTE ((uint32_t)((uint8_t *)&GPIOE->ODR + 1))
#define SYNC_GPIO_PORT GPIOD
#define HSYNC_PIN 15u
#define VSYNC_PIN 14u

#define SYNC_GPIO_CLK_ENABLE()  __HAL_RCC_GPIOD_CLK_ENABLE()
#define VIDEO_GPIO_CLK_ENABLE() __HAL_RCC_GPIOE_CLK_ENABLE()

#endif

#endif  // VGA_VGACONFIG_H
