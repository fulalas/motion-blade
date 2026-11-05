#include "y4m.h"

#include <stdlib.h>
#include <string.h>

bool y4m_frame_size(const char *header, int *width, int *height)
{
    const char *w = strstr(header, " W");
    const char *h = strstr(header, " H");
    const char *c = strstr(header, " C");
    *width = w ? atoi(w + 2) : 0;
    *height = h ? atoi(h + 2) : 0;
    bool bad_chroma = c && (strncmp(c + 2, "420", 3) || strstr(c, "p1"));
    return *width > 0 && *height > 0 && *width % 2 == 0 && *height % 2 == 0 && !bad_chroma;
}
