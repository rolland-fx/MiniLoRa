// timegm() shim for ESP-IDF newlib (used by vendored minmea).
// Compiled only into minmea.c via -include this header + -Dtimegm=...
#ifndef MINILORA_TIMEGM_COMPAT_H
#define MINILORA_TIMEGM_COMPAT_H

#include <time.h>

static inline time_t minilora_timegm(struct tm *tm)
{
    // Inverse of gmtime: days since epoch algorithm (Howard Hinnant's).
    long long y = tm->tm_year + 1900;
    long long m = tm->tm_mon + 1;
    long long d = tm->tm_mday;
    m -= 3;
    if (m < 0) {
        m += 12;
        y -= 1;
    }
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * m + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400LL + tm->tm_hour * 3600LL +
                    tm->tm_min * 60LL + tm->tm_sec);
}

#endif
