#ifndef __BOOTLOADER_H_
#define __BOOTLOADER_H_
 
#include "main.h"
#include "stm32f4xx.h"
 
typedef  void (*pFunction)(void);
 
/*!
* @brief 跳转到应用程序段
*        执行条件：无
* @param[in1] : 用户代码起始地址.
*
* @retval: 无
*/
 
#define FLASH_JUMP_ADDR							(0x08008000)
 
void jump_to_app(uint32_t app_addr);
 
 
#endif