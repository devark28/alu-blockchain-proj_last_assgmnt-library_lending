#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stddef.h>

void watch_for_interrupt(void);
bool was_interrupted(void);
bool read_line(char *buffer, size_t size);
bool prompt_passphrase(const char *label, char *buffer, size_t size);

#endif
