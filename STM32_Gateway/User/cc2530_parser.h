#ifndef __CC2530_PARSER_H
#define __CC2530_PARSER_H

#include "stm32f10x.h"

void CC2530_Init(void);
void CC2530_ParseFrame(uint8_t *data, uint16_t length);
void CC2530_HandleByte(uint8_t byte);

#endif /* __CC2530_PARSER_H */