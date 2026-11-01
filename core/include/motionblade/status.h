#ifndef MOTIONBLADE_STATUS_H
#define MOTIONBLADE_STATUS_H

typedef enum mb_status {
    MB_SUCCESS = 0,
    MB_ERROR_NO_MEMORY,
    MB_ERROR_UNSUPPORTED,
    MB_ERROR_DEVICE,
    MB_ERROR_BUSY,
    MB_ERROR_INVALID,
} mb_status;

const char *mb_status_name(mb_status status);

#endif
