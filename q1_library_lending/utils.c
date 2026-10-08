#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Splits line in place on delimiter and returns how many fields were found.
 * The last field keeps any remaining delimiters, so a line with too many
 * fields is caught by the caller's length checks instead of being dropped.
 */
int split_line(char *line, char delimiter, char *fields[], int max_fields)
{
    int count = 1;
    fields[0] = line;

    while (count < max_fields) {
        char *separator = strchr(fields[count - 1], delimiter);
        if (separator == NULL) {
            break;
        }
        *separator = '\0';
        fields[count] = separator + 1;
        count++;
    }
    return count;
}

bool copy_field(char *dest, size_t dest_size, const char *src)
{
    size_t length = strlen(src);
    if (length >= dest_size) {
        return false;
    }
    memcpy(dest, src, length + 1);
    return true;
}

void trim_whitespace(char *text)
{
    size_t start = 0;
    while (isspace((unsigned char)text[start])) {
        start++;
    }

    size_t end = strlen(text);
    while (end > start && isspace((unsigned char)text[end - 1])) {
        end--;
    }

    memmove(text, text + start, end - start);
    text[end - start] = '\0';
}

static bool file_ends_with_newline(FILE *file)
{
    if (fseek(file, -1, SEEK_END) != 0) {
        return true;
    }
    return fgetc(file) == '\n';
}

/* A file edited by hand may lack its final newline, so one is added first. */
bool append_line_to_file(const char *path, const char *line)
{
    FILE *file = fopen(path, "a+");
    if (file == NULL) {
        fprintf(stderr, "ERROR: cannot open %s (%s)\n", path, strerror(errno));
        return false;
    }

    bool needs_newline = !file_ends_with_newline(file);
    fseek(file, 0, SEEK_END);
    if (needs_newline) {
        fputc('\n', file);
    }
    fprintf(file, "%s\n", line);

    if (fclose(file) != 0) {
        fprintf(stderr, "ERROR: cannot write %s (%s)\n", path, strerror(errno));
        return false;
    }
    return true;
}

/*
 * printf's %-*s pads by bytes, which misaligns names such as "Ngũgĩ" whose
 * accented letters take two bytes in UTF-8. Continuation bytes (10xxxxxx)
 * are skipped so the width counts characters instead.
 */
void print_padded(const char *text, int width)
{
    int characters = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80) {
            characters++;
        }
    }
    printf("%s", text);
    for (int i = characters; i < width; i++) {
        putchar(' ');
    }
}

void format_time(time_t timestamp, char buffer[TIME_TEXT_SIZE])
{
    if (timestamp == 0) {
        copy_field(buffer, TIME_TEXT_SIZE, "-");
        return;
    }
    strftime(buffer, TIME_TEXT_SIZE, "%Y-%m-%d %H:%M:%S", localtime(&timestamp));
}
