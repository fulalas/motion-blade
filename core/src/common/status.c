#include "motionblade/status.h"

const char *mb_status_name(mb_status status)
{
    switch (status) {
    case MB_SUCCESS:
        return "ok";
    case MB_ERROR_NO_MEMORY:
        return "out of memory";
    case MB_ERROR_UNSUPPORTED:
        return "unsupported";
    case MB_ERROR_DEVICE:
        return "device error";
    case MB_ERROR_BUSY:
        return "busy";
    case MB_ERROR_INVALID:
        return "invalid argument";
    }
    return "unknown";
}
