#include "y4m.h"

#include <stdlib.h>
#include <string.h>

static bool is_8bit_420(const char *tag, size_t length)
{
    static const char *const tags[] = {"420", "420jpeg", "420mpeg2", "420paldv"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (strlen(tags[i]) == length && !strncmp(tag, tags[i], length)) {
            return true;
        }
    }
    return false;
}

bool y4m_frame_size(const char *header, int *width, int *height)
{
    const char *w = strstr(header, " W");
    const char *h = strstr(header, " H");
    const char *c = strstr(header, " C");
    *width = w ? atoi(w + 2) : 0;
    *height = h ? atoi(h + 2) : 0;
    bool bad_chroma = c && !is_8bit_420(c + 2, strcspn(c + 2, " \n"));
    return *width > 0 && *height > 0 && *width % 2 == 0 && *height % 2 == 0 && !bad_chroma;
}
