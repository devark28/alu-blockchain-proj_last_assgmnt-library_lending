#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define TIME_TEXT_SIZE 20
#define MAX_TIME_VALUE 32503680000LL

int split_line(char *line, char delimiter, char *fields[], int max_fields);
bool copy_field(char *dest, size_t dest_size, const char *src);
void trim_whitespace(char *text);
bool append_line_to_file(const char *path, const char *line);
void print_padded(const char *text, int width);
bool parse_number(const char *text, long long min, long long max, long long *value);
void format_time(time_t timestamp, char buffer[TIME_TEXT_SIZE]);

#endif
