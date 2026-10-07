// usb_audio_l7.cpp — L7's USB audio (USB Audio Class 2.0): see usb_audio_l7.h.
// tools/teensy_msc/patch.py copies it into the core (cores/teensy4).
#include <Arduino.h>
#include "usb_dev.h"
#include "usb_audio.h"
#include "AudioStream.h"
#include <string.h>

#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2)

extern volatile uint8_t usb_high_speed;

uint8_t usb_audio_receive_setting = 0;
uint8_t usb_audio_transmit_setting = 0;

#define ROUND32(n) (((n) + 31) & ~31)
#define TX_PACKET_MAX ROUND32(AUDIO_TX_SIZE_480 > AUDIO_TX_SIZE_12 ? AUDIO_TX_SIZE_480 : AUDIO_TX_SIZE_12)
#define RX_PACKET_MAX ROUND32(AUDIO_RX_SIZE_480 > AUDIO_RX_SIZE_12 ? AUDIO_RX_SIZE_480 : AUDIO_RX_SIZE_12)

static transfer_t tx_transfer __attribute__ ((used, aligned(32)));
static transfer_t rx_transfer __attribute__ ((used, aligned(32)));
static transfer_t sync_transfer __attribute__ ((used, aligned(32)));
DMAMEM static uint8_t tx_packet[TX_PACKET_MAX] __attribute__ ((aligned(32)));
DMAMEM static uint8_t rx_packet[RX_PACKET_MAX] __attribute__ ((aligned(32)));
DMAMEM static uint32_t sync_value[8] __attribute__ ((aligned(32)));   // (a cache line of its own)

// The servos: a ring's smoothed fill, against its mark, sets the rate asked for
// (frames a second per frame off; at most RATE_TRIM_MAX either way). Their time
// constant ~2 s, the smoothing's ~0.1 s.
static constexpr float RATE_GAIN = 0.5f;
static constexpr float RATE_TRIM_MAX = 200.0f;      // (~0.45 %)
static constexpr float FILL_SMOOTH = 1.0f / 32;     // (a block)

// ---- To the computer: a ring of whole blocks (written a block at a time, so
// always in one piece), sent a packet at a time.
static constexpr uint32_t TX_RING = 2 * AUDIO_BLOCK_SAMPLES;                    // frames
static constexpr uint32_t TX_MARK = AUDIO_BLOCK_SAMPLES + 40;                   // after a block written (~1 ms to spare before it)
DMAMEM static int16_t tx_ring[TX_RING * AUDIO_TX_CHANNELS_480] __attribute__ ((aligned(32)));
static volatile uint32_t tx_head = 0;      // frames written (the audio update's; always whole blocks)
static volatile uint32_t tx_tail = 0;      // frames sent (the USB interrupt's)
static volatile bool tx_started = false;   // sending from the ring (else silence, until it reaches its mark)
static volatile float tx_rate = AUDIO_SAMPLE_RATE_EXACT;
static float tx_acc = 0;                   // fractional frames owed
static float tx_fill = TX_MARK;
static volatile uint32_t tx_underruns = 0, tx_dropped = 0;

// ---- From the computer: stereo frames as they come, read a block at a time.
static constexpr uint32_t RX_RING = 4 * AUDIO_BLOCK_SAMPLES;
static constexpr uint32_t RX_MARK = AUDIO_BLOCK_SAMPLES + 40;                   // before a block read
DMAMEM static uint32_t rx_ring[RX_RING] __attribute__ ((aligned(32)));
static volatile uint32_t rx_head = 0;      // frames received (the USB interrupt's)
static volatile uint32_t rx_tail = 0;      // frames read (the audio update's)
static volatile bool rx_flush = true;      // (set by the USB interrupt: the reader starts over)
static bool rx_started = false;
static volatile float rx_rate = AUDIO_SAMPLE_RATE_EXACT;
static float rx_fill = RX_MARK;
static volatile uint32_t rx_underruns = 0, rx_overruns = 0;

static void tx_event(transfer_t *t)
{
	const uint32_t hs = usb_high_speed;
	const uint32_t channels = hs ? AUDIO_TX_CHANNELS_480 : AUDIO_TX_CHANNELS_12;
	const uint32_t maxFrames = (hs ? AUDIO_TX_SIZE_480 : AUDIO_TX_SIZE_12) / (2 * channels);
	uint32_t n = 0;
	if (usb_audio_transmit_setting) {
		tx_acc += tx_rate * (hs ? 1.0f / 8000 : 1.0f / 1000);   // (a packet each microframe; at full speed, each frame)
		n = (uint32_t)tx_acc;
		tx_acc -= n;
		if (n > maxFrames) n = maxFrames;
	}
	uint32_t tail = tx_tail;
	const uint32_t head = tx_head;
	if (!tx_started && head - tail >= TX_MARK) {        // filled to the mark: from its newest TX_MARK frames on
		tail = head - TX_MARK;
		tx_started = true;
	}
	int16_t *dst = (int16_t *)tx_packet;
	for (uint32_t k = 0; k < n; k++, dst += channels) {
		if (tx_started && tail != head) {
			memcpy(dst, tx_ring + (tail % TX_RING) * AUDIO_TX_CHANNELS_480, channels * 2);
			tail++;
		} else {
			memset(dst, 0, channels * 2);
			if (tx_started) {                            // ran dry: silence until the mark again
				tx_started = false;
				tx_underruns++;
			}
		}
	}
	tx_tail = tail;
	uint32_t len = n * channels * 2;
	usb_prepare_transfer(&tx_transfer, tx_packet, len, 0);
	arm_dcache_flush_delete(tx_packet, ROUND32(len ? len : 32));
	usb_transmit(AUDIO_TX_ENDPOINT, &tx_transfer);
}

static void rx_event(transfer_t *t)
{
	const uint32_t size = usb_high_speed ? AUDIO_RX_SIZE_480 : AUDIO_RX_SIZE_12;
	if (t) {
		uint32_t len = size - ((rx_transfer.status >> 16) & 0x7FFF);
		arm_dcache_delete(rx_packet, RX_PACKET_MAX);
		const uint32_t *src = (const uint32_t *)rx_packet;
		uint32_t frames = len / 4, head = rx_head;
		if (usb_audio_receive_setting) {
			for (uint32_t k = 0; k < frames; k++) {
				if (head - rx_tail >= RX_RING) {             // no room (the reader stopped): the rest dropped
					rx_overruns++;
					break;
				}
				rx_ring[head % RX_RING] = src[k];
				head++;
			}
			__asm__ volatile("" ::: "memory");
			rx_head = head;
		}
	}
	usb_prepare_transfer(&rx_transfer, rx_packet, size, 0);
	arm_dcache_delete(rx_packet, RX_PACKET_MAX);
	usb_receive(AUDIO_RX_ENDPOINT, &rx_transfer);
}

static void sync_event(transfer_t *t)
{
	// USB 2.0, 5.12.4.2: frames per microframe as 16.16 (high speed), per frame as 10.14 (full speed)
	const float rate = rx_rate;
	uint32_t nbytes;
	if (usb_high_speed) {
		sync_value[0] = (uint32_t)(rate * (65536.0f / 8000) + 0.5f);
		nbytes = 4;
	} else {
		sync_value[0] = (uint32_t)(rate * (16384.0f / 1000) + 0.5f);
		nbytes = 3;
	}
	usb_prepare_transfer(&sync_transfer, sync_value, nbytes, 0);
	arm_dcache_flush(sync_value, 32);
	usb_transmit(AUDIO_SYNC_ENDPOINT, &sync_transfer);
}

void usb_audio_configure(void)
{
	usb_audio_transmit_setting = 0;
	usb_audio_receive_setting = 0;
	tx_tail = tx_head;
	tx_started = false;
	rx_flush = true;
	const uint32_t hs = usb_high_speed;
	memset(&rx_transfer, 0, sizeof(rx_transfer));
	usb_config_rx_iso(AUDIO_RX_ENDPOINT, hs ? AUDIO_RX_SIZE_480 : AUDIO_RX_SIZE_12, 1, rx_event);
	rx_event(NULL);
	memset(&sync_transfer, 0, sizeof(sync_transfer));
	usb_config_tx_iso(AUDIO_SYNC_ENDPOINT, hs ? 4 : 3, 1, sync_event);
	sync_event(NULL);
	memset(&tx_transfer, 0, sizeof(tx_transfer));
	usb_config_tx_iso(AUDIO_TX_ENDPOINT, hs ? AUDIO_TX_SIZE_480 : AUDIO_TX_SIZE_12, 1, tx_event);
	tx_event(NULL);
}

void usb_audio_set_interface(uint32_t which, uint32_t alt)
{
	if (which == 1) {
		usb_audio_transmit_setting = alt;
		tx_tail = tx_head;                                  // (what's left from before: dropped)
		tx_started = false;
		tx_acc = 0;
	} else {
		usb_audio_receive_setting = alt;
		rx_flush = true;
	}
}

static uint8_t reply[16];   // (DTCM: endpoint 0 sends it as it is)

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

int usb_audio_class_request(uint32_t requestAndType, uint32_t value, uint32_t index, const uint8_t **out, uint32_t *len)
{
	// Only the clock source has controls: its frequency (fixed) and whether it's valid.
	if ((index & 0xFF) != AUDIO_INTERFACE || (index >> 8) != AUDIO_CLOCK_ID) return 0;
	const uint32_t hz = 44100;
	if (requestAndType == 0x01A1 && value == 0x0100) {          // GET CUR, sampling frequency
		put32(reply, hz);
		*len = 4;
	} else if (requestAndType == 0x01A1 && value == 0x0200) {   // GET CUR, clock valid
		reply[0] = 1;
		*len = 1;
	} else if (requestAndType == 0x02A1 && value == 0x0100) {   // GET RANGE, sampling frequency: just the one
		reply[0] = 1;
		reply[1] = 0;
		put32(reply + 2, hz);
		put32(reply + 6, hz);
		put32(reply + 10, 0);
		*len = 14;
	} else {
		return 0;
	}
	*out = reply;
	return 1;
}

int16_t *usb_audio_tx_block(void)
{
	if (!usb_audio_transmit_setting) return NULL;
	if (tx_head - tx_tail + AUDIO_BLOCK_SAMPLES > TX_RING) {   // the computer not taking it
		tx_dropped++;
		return NULL;
	}
	return tx_ring + (tx_head % TX_RING) * AUDIO_TX_CHANNELS_480;
}

void usb_audio_tx_written(void)
{
	__asm__ volatile("" ::: "memory");
	const uint32_t head = tx_head + AUDIO_BLOCK_SAMPLES;
	tx_head = head;
	if (!tx_started) return;
	tx_fill += ((float)(head - tx_tail) - tx_fill) * FILL_SMOOTH;
	float trim = (tx_fill - TX_MARK) * RATE_GAIN;            // fuller than the mark: sent a little faster
	tx_rate = AUDIO_SAMPLE_RATE_EXACT + constrain(trim, -RATE_TRIM_MAX, RATE_TRIM_MAX);
}

int usb_audio_rx_block(uint32_t *frames)
{
	if (rx_flush) {
		rx_flush = false;
		rx_tail = rx_head;
		rx_started = false;
	}
	if (!usb_audio_receive_setting) return 0;
	uint32_t tail = rx_tail;
	const uint32_t fill = rx_head - tail;
	if (!rx_started) {
		if (fill < RX_MARK) return 0;
		tail += fill - RX_MARK;                                 // (from the mark on)
		rx_started = true;
		rx_fill = RX_MARK;
	} else if (fill < AUDIO_BLOCK_SAMPLES) {                  // ran dry: nothing until the mark again
		rx_started = false;
		rx_underruns++;
		return 0;
	} else {
		rx_fill += ((float)fill - rx_fill) * FILL_SMOOTH;
		float trim = (RX_MARK - rx_fill) * RATE_GAIN;          // emptier than the mark: asked for a little more
		rx_rate = AUDIO_SAMPLE_RATE_EXACT + constrain(trim, -RATE_TRIM_MAX, RATE_TRIM_MAX);
	}
	for (uint32_t k = 0; k < AUDIO_BLOCK_SAMPLES; k++) frames[k] = rx_ring[(tail + k) % RX_RING];
	__asm__ volatile("" ::: "memory");
	rx_tail = tail + AUDIO_BLOCK_SAMPLES;
	return 1;
}

void usb_audio_l7_get_stats(struct usb_audio_l7_stats *s)
{
	s->txUnderruns = tx_underruns;
	s->txDropped = tx_dropped;
	s->rxUnderruns = rx_underruns;
	s->rxOverruns = rx_overruns;
	s->txFill = tx_fill;
	s->rxFill = rx_fill;
	s->txRate = tx_rate;
	s->rxRate = rx_rate;
}

#endif
