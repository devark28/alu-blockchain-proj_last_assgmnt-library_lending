#include "input.h"
#include "utils.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted = 0;

static void handle_interrupt(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

/*
 * Ctrl-C only sets a flag, because saving files or freeing keys inside a
 * signal handler is unsafe. SA_RESTART is left off so a blocked read returns
 * at once and the main loop can shut down cleanly.
 */
void watch_for_interrupt(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_interrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
}

bool was_interrupted(void)
{
    return interrupted != 0;
}

/* Reads one line from stdin, discarding whatever does not fit in buffer. */
bool read_line(char *buffer, size_t size)
{
    if (was_interrupted() || fgets(buffer, (int)size, stdin) == NULL) {
        buffer[0] = '\0';
        return false;
    }

    size_t length = strcspn(buffer, "\n");
    if (buffer[length] != '\n') {
        int extra;
        while ((extra = getchar()) != '\n' && extra != EOF) {
        }
    }
    buffer[length] = '\0';
    trim_whitespace(buffer);
    return true;
}

/*
 * Terminal echo is turned off while the passphrase is typed. It is always
 * turned back on, including when Ctrl-C interrupts the read.
 */
bool prompt_passphrase(const char *label, char *buffer, size_t size)
{
    struct termios original_settings;
    bool is_terminal = tcgetattr(STDIN_FILENO, &original_settings) == 0;

    if (is_terminal) {
        struct termios hidden_settings = original_settings;
        hidden_settings.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &hidden_settings);
    }

    printf("%s", label);
    fflush(stdout);
    bool read_ok = read_line(buffer, size);

    if (is_terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_settings);
        printf("\n");
    }
    return read_ok;
}
