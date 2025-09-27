#ifndef __BOOTLOADER_H_
#define __BOOTLOADER_H_
 
#include "main.h"
#include "stm32f4xx.h"
 
typedef  void (*pFunction)(void);
 
/*!
* @brief ��ת��Ӧ�ó����
*        ִ����������
* @param[in1] : �û�������ʼ��ַ.
*
* @retval: ��
*/
 
#define FLASH_JUMP_ADDR							(0x08008000)
 
void jump_to_app(uint32_t app_addr);
 
 
#endif