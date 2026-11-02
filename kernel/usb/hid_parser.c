// kernel/usb/hid_parser.c
#include "hid_parser.h"
#include "../klog.h"
#include "../string.h"
#include "../uaccess.h"
#include "usb_hid.h"


#define HID_ITEM_LONG 0xFE

static uint32_t hid_extract_value(const uint8_t *data, int size,
                                  int is_signed) {
  uint32_t val = 0;
  for (int i = 0; i < size; i++)
    val |= (uint32_t)data[i] << (i * 8);
  if (is_signed && size < 4) {
    uint32_t sign_bit = 1u << (size * 8 - 1);
    if (val & sign_bit)
      val |= ~((1u << (size * 8)) - 1);
  }
  return val;
}

// Bit offset dentro de SU tipo de report. Los reports de Input, Output y
// Feature son independientes según HID 1.11 §6.2.2.5 y §6.2.2.8.
static void hid_add_fields(hid_report_t *r, uint32_t *bits,
                           uint8_t *bit_offset_out, uint32_t g_usage_page,
                           int32_t g_logical_min, int32_t g_logical_max,
                           uint32_t g_report_size, uint32_t g_report_count,
                           uint8_t g_report_id, uint8_t has_report_id,
                           uint32_t flags, uint32_t *l_usages, int l_num_usages,
                           uint32_t l_usage_min, uint32_t l_usage_max) {
  for (uint32_t c = 0; c < g_report_count; c++) {
    if (r->num_fields >= HID_MAX_FIELDS)
      return;
    hid_field_t *f = &r->fields[r->num_fields++];
    f->usage_page = g_usage_page;
    f->logical_min = g_logical_min;
    f->logical_max = g_logical_max;
    f->report_size = g_report_size;
    f->report_id = g_report_id;
    f->is_variable = (flags & 0x02) ? 1 : 0;
    f->is_relative = (flags & 0x04) ? 1 : 0;
    f->is_constant = (flags & 0x01) ? 1 : 0;
    f->bit_offset = *bits;

    if (l_num_usages > 0) {
      uint32_t usage_count = (uint32_t)l_num_usages;
      uint32_t usage_index = c < usage_count ? c : usage_count - 1;
      f->usage = l_usages[usage_index];
    }
    else if (l_usage_min && l_usage_max)
      f->usage = l_usage_min + c;

    if (f->usage_page == 0x09)
      f->is_button = 1;
    *bits += g_report_size;
  }
  (void)has_report_id;
  (void)bit_offset_out;
}

int hid_parse_report_descriptor(const uint8_t *desc, int desc_len,
                                hid_report_t *report) {
  memset(report, 0, sizeof(*report));

  uint32_t g_usage_page = 0;
  int32_t g_logical_min = 0;
  int32_t g_logical_max = 0;
  uint32_t g_report_size = 0;
  uint32_t g_report_count = 0;
  uint8_t g_report_id = 0;
  uint8_t has_report_id = 0;

  uint32_t l_usages[32];
  int l_num_usages = 0;
  uint32_t l_usage_min = 0, l_usage_max = 0;

  /* Los offsets de Input son por Report ID (HID 1.11 6.2.2.7): cada ID
   * empieza en el bit 0 de su propio informe. */
  uint32_t id_bits[256] = {0};
  uint32_t in_bits = 0, in_max_bits = 0, out_bits = 0, feat_bits = 0;
  int i = 0;

  while (i < desc_len) {
    uint8_t prefix = desc[i++];
    if (prefix == HID_ITEM_LONG) {
      if (i + 2 > desc_len)
        break;
      uint8_t data_size = desc[i++];
      i++;
      i += data_size;
      continue;
    }
    uint8_t size = prefix & 0x03;
    if (size == 3)
      size = 4;
    uint8_t type = (prefix >> 2) & 0x03;
    uint8_t tag = (prefix >> 4) & 0x0F;
    if (i + size > desc_len)
      break;
    uint32_t data = hid_extract_value(
        &desc[i], size, (type == HID_ITEM_TYPE_GLOBAL && tag == 0x1));
    i += size;

    if (type == HID_ITEM_TYPE_MAIN) {
      if (tag == 0x8) { // Input
        in_bits = id_bits[g_report_id];
        hid_add_fields(report, &in_bits, NULL, g_usage_page, g_logical_min,
                       g_logical_max, g_report_size, g_report_count,
                       g_report_id, has_report_id, data, l_usages, l_num_usages,
                       l_usage_min, l_usage_max);
        id_bits[g_report_id] = in_bits;
        if (in_bits > in_max_bits)
          in_max_bits = in_bits;
        l_num_usages = 0;
        l_usage_min = l_usage_max = 0;
      } else if (tag == 0x9) { // Output — bits SEPARADOS
        // No llama a hid_add_fields para no almacenar los LEDs en
        // fields (no los necesitamos, y confunden el auto-repeat).
        out_bits += g_report_size * g_report_count;
        l_num_usages = 0;
        l_usage_min = l_usage_max = 0;
      } else if (tag == 0xB) { // Feature — bits SEPARADOS
        feat_bits += g_report_size * g_report_count;
        l_num_usages = 0;
        l_usage_min = l_usage_max = 0;
      } else if (tag == 0xA || tag == 0xC) { // Collection / End
        l_num_usages = 0;
        l_usage_min = l_usage_max = 0;
      }
    } else if (type == HID_ITEM_TYPE_GLOBAL) {
      switch (tag) {
      case 0x0:
        g_usage_page = data;
        break;
      case 0x1:
        g_logical_min = (int32_t)data;
        break;
      case 0x2:
        g_logical_max = (int32_t)data;
        break;
      case 0x7:
        g_report_size = data;
        break;
      case 0x8:
        g_report_id = data & 0xFF;
        has_report_id = 1;
        break;
      case 0x9:
        g_report_count = data;
        break;
      }
    } else if (type == HID_ITEM_TYPE_LOCAL) {
      switch (tag) {
      case 0x0:
        if (l_num_usages < 32)
          l_usages[l_num_usages++] = data;
        break;
      case 0x1:
        l_usage_min = data;
        break;
      case 0x2:
        l_usage_max = data;
        break;
      }
    }
  }

  report->has_report_id = has_report_id;
  report->report_id = g_report_id;
  report->input_len = (in_max_bits + 7) / 8;
  report->output_len = (out_bits + 7) / 8;
  report->feature_len = (feat_bits + 7) / 8;
  return 0;
}