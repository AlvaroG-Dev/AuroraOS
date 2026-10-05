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

// [4.2] Devuelve el epoch UNIX actual (segundos desde 1970-01-01 00:00 UTC).
// Devuelve 0 si el RTC no se puede leer.
int64_t rtc_get_epoch(void);

// Escribe la fecha/hora al RTC CMOS. Devuelve 1 si OK, 0 si falla.
int rtc_set_datetime(const rtc_datetime_t *dt);

int rtc_set_epoch(int64_t epoch);