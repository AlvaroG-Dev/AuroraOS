// kernel/rtc.h
#pragma once
#include <stdint.h>

// Fecha/hora leida del RTC CMOS. year es el año completo (ej: 2026).
typedef struct {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t month;
  uint16_t year;
  uint8_t century;
} rtc_datetime_t;

void rtc_init(void);
int rtc_read_datetime(rtc_datetime_t *out);
void rtc_format_time(const rtc_datetime_t *dt, char *out, int out_len);
void rtc_format_date(const rtc_datetime_t *dt, char *out, int out_len);
