#ifndef HTTP_SNTP_SYNC_H
#define HTTP_SNTP_SYNC_H

#pragma once
#include <stdint.h>

void     http_time_sync_start(void);
uint32_t http_time_get_utc(uint16_t *out_ms);


#endif /* SNTP_SYNC_H */