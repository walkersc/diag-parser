#ifndef DIAG_INPUT_H
#define DIAG_INPUT_H

#include <stdint.h>

void diag_init(unsigned start_sid, unsigned start_cid, char *filename);
void handle_diag(uint8_t *msg, unsigned len);
void diag_destroy();

#endif
