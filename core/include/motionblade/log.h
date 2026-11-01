#ifndef MOTIONBLADE_LOG_H
#define MOTIONBLADE_LOG_H

typedef enum mb_log_level {
    MB_LOG_ERROR,
    MB_LOG_WARNING,
    MB_LOG_INFO,
    MB_LOG_VERBOSE,
} mb_log_level;

typedef struct mb_logger {
    void (*write)(void *context, mb_log_level level, const char *message);
    void *context;
    mb_log_level max_level;
} mb_logger;

mb_logger mb_logger_stderr(mb_log_level max_level);
void mb_log(const mb_logger *logger, mb_log_level level, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

#endif
