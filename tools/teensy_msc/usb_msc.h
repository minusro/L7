/* usb_msc.h — USB mass storage (bulk-only, SCSI transparent) for the Teensy 4
 * core, added by tools/teensy_msc/patch.py (not part of Teensyduino).
 *
 * The device shows a block device (the SD card) to the computer as a disk.
 * usb_msc_poll() runs the protocol, a command at a time: call it as often as
 * possible from the sketch (the data stages block on the card).
 */
#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t (*sector_count)(void);                                      // 512-byte sectors
	int (*read)(uint32_t lba, uint8_t *dst, uint32_t count);              // 1: ok
	int (*write)(uint32_t lba, const uint8_t *src, uint32_t count);       // 1: ok
	void (*sync)(void);                                                  // everything written to the card
} usb_msc_media_t;

void usb_msc_set_media(const usb_msc_media_t *media);

// Run the protocol: a command's worth per call (data stages included).
void usb_msc_poll(void);

// Called by the USB core (usb.c).
void usb_msc_configure(void);
void usb_msc_reset_request(void);

// What the host has done: 1 once it has sent anything since configure; and
// 1 once it has ejected the disk (START STOP UNIT).
int usb_msc_host_seen(void);
// Forgets what the host has done (seen, ejected), for the next time a medium is shown.
void usb_msc_clear_host_state(void);
// The last command's opcode and how many there have been (for logging).
extern volatile uint8_t usb_msc_last_op;
extern volatile uint32_t usb_msc_commands;
extern volatile uint32_t usb_msc_failed, usb_msc_last_sense, usb_msc_broken, usb_msc_restarts, usb_msc_reads, usb_msc_writes;
extern volatile uint8_t usb_msc_failed_op;
int usb_msc_ejected(void);
// Sectors read / written so far (for a screen).
uint32_t usb_msc_sectors_read(void);
uint32_t usb_msc_sectors_written(void);

#ifdef __cplusplus
}
#endif

#endif
