// kernel/rtc.c
#include "rtc.h"
#include "serial.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_REG_SECONDS 0x00
#define RTC_REG_MINUTES 0x02
#define RTC_REG_HOURS   0x04
#define RTC_REG_DAY     0x07
#define RTC_REG_MONTH   0x08
#define RTC_REG_YEAR    0x09
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B
#define RTC_REG_CENTURY 0x32

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "dN"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
  return ret;
}

static uint8_t cmos_read(uint8_t reg) {
  outb(CMOS_ADDR, reg | 0x80); // Mantener NMI deshabilitada durante el acceso CMOS.
  return inb(CMOS_DATA);
}

static int rtc_update_in_progress(void) {
  return (cmos_read(RTC_REG_STATUS_A) & 0x80) != 0;
}

static uint8_t rtc_bcd_to_bin(uint8_t value) {
  return (uint8_t)((value & 0x0F) + ((value / 16) * 10));
}

static int rtc_wait_stable(void) {
  for (int i = 0; i < 100000; i++) {
    if (!rtc_update_in_progress()) return 1;
  }
  return 0;
}

static void rtc_read_raw(rtc_datetime_t *dt, uint8_t *status_b, uint8_t *raw_hour) {
  dt->second = cmos_read(RTC_REG_SECONDS);
  dt->minute = cmos_read(RTC_REG_MINUTES);
  *raw_hour = cmos_read(RTC_REG_HOURS);
  dt->hour = *raw_hour;
  dt->day = cmos_read(RTC_REG_DAY);
  dt->month = cmos_read(RTC_REG_MONTH);
  dt->year = cmos_read(RTC_REG_YEAR);
  dt->century = cmos_read(RTC_REG_CENTURY);
  *status_b = cmos_read(RTC_REG_STATUS_B);
}

static int rtc_same_snapshot(const rtc_datetime_t *a, const rtc_datetime_t *b) {
  return a->second == b->second &&
         a->minute == b->minute &&
         a->hour == b->hour &&
         a->day == b->day &&
         a->month == b->month &&
         a->year == b->year &&
         a->century == b->century;
}

int rtc_read_datetime(rtc_datetime_t *out) {
  if (!out) return 0;

  rtc_datetime_t a;
  rtc_datetime_t b;
  uint8_t status_b = 0;
  uint8_t raw_hour = 0;

  for (int attempt = 0; attempt < 8; attempt++) {
    if (!rtc_wait_stable()) return 0;
    rtc_read_raw(&a, &status_b, &raw_hour);
    if (!rtc_wait_stable()) return 0;
    rtc_read_raw(&b, &status_b, &raw_hour);

    if (rtc_same_snapshot(&a, &b)) {
      *out = b;
      uint8_t hour_raw = raw_hour;
      int pm = (hour_raw & 0x80) != 0;

      if (!(status_b & 0x04)) {
        out->second = rtc_bcd_to_bin(out->second);
        out->minute = rtc_bcd_to_bin(out->minute);
        out->hour = rtc_bcd_to_bin(hour_raw & 0x7F);
        out->day = rtc_bcd_to_bin(out->day);
        out->month = rtc_bcd_to_bin(out->month);
        out->year = rtc_bcd_to_bin((uint8_t)out->year);
        if (out->century) out->century = rtc_bcd_to_bin(out->century);
      } else {
        out->hour = hour_raw & 0x7F;
      }

      if (!(status_b & 0x02) && pm) {
        out->hour = (uint8_t)((out->hour + 12) % 24);
      }

      if (out->century) {
        out->year = (uint16_t)(out->century * 100 + out->year);
      } else {
        out->year = (uint16_t)(2000 + out->year);
      }
      return 1;
    }
  }

  return 0;
}

static void write_two_digits(char *out, uint8_t value) {
  out[0] = (char)('0' + (value / 10));
  out[1] = (char)('0' + (value % 10));
}

void rtc_format_time(const rtc_datetime_t *dt, char *out, int out_len) {
  if (!out || out_len <= 0) return;
  if (!dt || out_len < 6) {
    out[0] = '\0';
    return;
  }

  write_two_digits(&out[0], dt->hour);
  out[2] = ':';
  write_two_digits(&out[3], dt->minute);
  out[5] = '\0';
}

void rtc_format_date(const rtc_datetime_t *dt, char *out, int out_len) {
  if (!out || out_len <= 0) return;
  if (!dt || out_len < 11) {
    out[0] = '\0';
    return;
  }

  write_two_digits(&out[0], dt->day);
  out[2] = '/';
  write_two_digits(&out[3], dt->month);
  out[5] = '/';
  out[6] = (char)('0' + ((dt->year / 1000) % 10));
  out[7] = (char)('0' + ((dt->year / 100) % 10));
  out[8] = (char)('0' + ((dt->year / 10) % 10));
  out[9] = (char)('0' + (dt->year % 10));
  out[10] = '\0';
}

void rtc_init(void) {
  rtc_datetime_t now;
  if (!rtc_read_datetime(&now)) {
    serial_puts("[RTC] No se pudo leer fecha/hora del CMOS\n");
    return;
  }

  serial_puts("[RTC] CMOS RTC detectado: ");
  serial_putn(now.hour, 10, 2);
  serial_puts(":");
  serial_putn(now.minute, 10, 2);
  serial_puts(" ");
  serial_putn(now.day, 10, 2);
  serial_puts("/");
  serial_putn(now.month, 10, 2);
  serial_puts("/");
  serial_putn(now.year, 10, 4);
  serial_puts("\n");
}
