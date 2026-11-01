#include "motionblade/log.h"

#include <stdarg.h>
#include <stdio.h>

enum { MESSAGE_CAPACITY = 256 };

static void write_stderr(void *context, mb_log_level level, const char *message)
{
    (void)context;
    (void)level;
    fprintf(stderr, "motionblade: %s\n", message);
}

mb_logger mb_logger_stderr(mb_log_level max_level)
{
    return (mb_logger){write_stderr, NULL, max_level};
}

void mb_log(const mb_logger *logger, mb_log_level level, const char *format, ...)
{
    if (level > logger->max_level || !logger->write) {
        return;
    }
    char message[MESSAGE_CAPACITY];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    logger->write(logger->context, level, message);
}
