#include "motionblade/backend.h"

#include <string.h>

typedef void (*any_function)(void);

bool mb_backend_is_complete(const mb_backend *backend)
{
    if (!backend->ops || backend->max_pending < 1) {
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)backend->ops;
    for (size_t offset = 0; offset < sizeof(mb_backend_ops); offset += sizeof(any_function)) {
        any_function entry;
        memcpy(&entry, bytes + offset, sizeof(entry));
        if (!entry) {
            return false;
        }
    }
    return true;
}
