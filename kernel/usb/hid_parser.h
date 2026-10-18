// kernel/usb/hid_parser.h
#ifndef KERNEL_USB_HID_PARSER_H
#define KERNEL_USB_HID_PARSER_H

#include <stdint.h>

#define HID_MAX_FIELDS 128

typedef enum {
  HID_ITEM_TYPE_MAIN = 0,
  HID_ITEM_TYPE_GLOBAL = 1,
  HID_ITEM_TYPE_LOCAL = 2,
} hid_item_type_t;

typedef struct {
  uint32_t usage_page;
  uint32_t usage;
  int32_t logical_min;
  int32_t logical_max;
  uint32_t report_size;
  uint32_t report_count;
  uint32_t bit_offset; // offset dentro de SU report (Input/Output/Feature)
  uint8_t report_id;
  uint8_t is_variable;
  uint8_t is_relative;
  uint8_t is_button;
  uint8_t is_constant;
} hid_field_t;

typedef struct {
  hid_field_t fields[HID_MAX_FIELDS];
  int num_fields;
  uint8_t has_report_id;
  uint8_t report_id; // el primer report ID usado
  int input_len;     // bytes del report de Input (sin contar Report ID)
  int output_len;
  int feature_len;
} hid_report_t;

int hid_parse_report_descriptor(const uint8_t *desc, int desc_len,
                                hid_report_t *report);

#endif