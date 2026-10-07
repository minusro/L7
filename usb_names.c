// usb_names.c — the M8's name over USB: "L7" (its USB MIDI device, and the
// serial port's description), instead of Teensyduino's "USB Serial". The
// core's default is weak; this replaces it (Teensyduino's usb_names.h). The
// IDs stay Teensyduino's, so uploading finds it as before.
#include "usb_names.h"

#define L7_PRODUCT_NAME     {'L', '7'}
#define L7_PRODUCT_NAME_LEN 2

struct usb_string_descriptor_struct usb_string_product_name = {
  2 + L7_PRODUCT_NAME_LEN * 2,
  3,
  L7_PRODUCT_NAME
};
