// usb_audio_l7.h — L7's USB audio (USB Audio Class 2.0), in place of Teensyduino's
// (the patched USB type defines AUDIO_UAC2; tools/teensy_msc/patch.py copies this
// into the core and has usb_audio.h include it).
//
// To the computer: AUDIO_TX_CHANNELS_480 channels (16: the main mix, then each
// track, stereo pairs; at full speed only the first AUDIO_TX_CHANNELS_12). From it:
// stereo. 44.1 kHz as far as the computer knows, 16 bits; both streams
// asynchronous, timed by L7's own clock (AUDIO_SAMPLE_RATE_EXACT): the frames sent
// each microframe follow what the audio library makes, and what comes in is
// asked for at that rate (the feedback endpoint).
//
// Both sides are rings between the audio library's update (a block at a time)
// and the USB interrupt (a packet at a time).
#pragma once

#include "usb_desc.h"
#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2)
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// usb.c's side
void usb_audio_configure(void);
void usb_audio_set_interface(uint32_t which, uint32_t alt);   // which: 1 to the computer, 2 from it
// A class request to the audio function (GET CUR / RANGE): 1 and the reply, or 0 (stalled).
int usb_audio_class_request(uint32_t requestAndType, uint32_t value, uint32_t index, const uint8_t **reply, uint32_t *len);
extern uint8_t usb_audio_receive_setting;    // the alternate settings chosen: 1 streaming, 0 not
extern uint8_t usb_audio_transmit_setting;

// The audio update's side (each once a block):
// Room for a block (AUDIO_BLOCK_SAMPLES frames of AUDIO_TX_CHANNELS_480 samples,
// interleaved) to the computer, then usb_audio_tx_written(); NULL: not streaming,
// or no room (the computer not taking it).
int16_t *usb_audio_tx_block(void);
void usb_audio_tx_written(void);
// A block from the computer (AUDIO_BLOCK_SAMPLES stereo frames, left | right << 16):
// 1, or 0 when none (not streaming, or not enough yet).
int usb_audio_rx_block(uint32_t *frames);

struct usb_audio_l7_stats {
	uint32_t txUnderruns, txDropped;           // to the computer: frames it asked for that weren't made yet; blocks with no room
	uint32_t rxUnderruns, rxOverruns;          // from it: blocks short; frames with no room
	float txFill, rxFill;                      // the rings' smoothed fill (frames: after a block written / before one read)
	float txRate, rxRate;                      // the rates asked for (frames a second)
};
void usb_audio_l7_get_stats(struct usb_audio_l7_stats *s);
#ifdef __cplusplus
}
#endif

#endif
