/* usb_msc.c — USB mass storage (bulk-only transport, SCSI) for the Teensy 4
 * core. Added by tools/teensy_msc/patch.py; modelled on usb_mtp.c.
 *
 * One command at a time, polled from the sketch (usb_msc_poll()): the
 * command block wrapper arrives on the OUT endpoint, its data stage goes
 * through a 16 KB buffer in 32-sector pieces, the status wrapper goes back on
 * the IN endpoint. Unsupported commands, and errors, answer with a stalled
 * endpoint and a failed status, as the spec says, so a host never hangs.
 *
 * Two ways to run it, by whether there is a medium (usb_msc_set_media()):
 *  - none: an empty card reader. Answers are only queued, never waited for
 *    (async), so it is safe to poll from a timer interrupt: the computer's
 *    probing is always served and nothing mounts.
 *  - a medium: reads and writes block on the card and the USB transfers, so
 *    call usb_msc_poll() from the main loop only.
 */
#include "usb_desc.h"  // (MSC_INTERFACE, the endpoints)
#include "usb_dev.h"
#include "usb_msc.h"
#include "core_pins.h"
#include <string.h>

#ifdef MSC_INTERFACE // defined by usb_dev.h -> usb_desc.h

extern volatile uint8_t usb_high_speed;
extern volatile uint8_t usb_configuration;

#define SECTOR 512
#define DATA_BYTES 16384

static uint8_t data_buf[DATA_BYTES] __attribute__ ((aligned(32)));
static uint8_t cbw_buf[512] __attribute__ ((aligned(32)));
static uint8_t csw_buf[32] __attribute__ ((aligned(32)));
static uint8_t small_buf[64] __attribute__ ((aligned(32)));
// The USB core links a new transfer behind the endpoint's last one, so one
// transfer_t must never be queued twice in a row (it would point at itself):
// each endpoint rotates through several, as the core's own classes do.
#define RXN 2
#define TXN 4
static transfer_t rx_xfer[RXN] __attribute__ ((used, aligned(32)));
static transfer_t tx_xfer[TXN] __attribute__ ((used, aligned(32)));
static uint8_t rx_head = 0, tx_head = 0;
static transfer_t *cbw_xfer = NULL;               // the receive for the next command block
static transfer_t *async_xfer[3];                 // (async: the queued answer's transfers: data, zero-length packet, status)
static uint8_t async_used = 0;
static uint8_t async_mode = 0;                    // answering without waiting
static volatile uint8_t need_init = 1;            // (re)start: set by the USB core, done by the poll
static uint32_t async_since = 0;

static const usb_msc_media_t *media = NULL;
static uint16_t pkt = 64;
static volatile uint8_t reset_requested = 0;
static uint8_t cbw_queued = 0, host_seen = 0, ejected = 0;
static uint32_t n_read = 0, n_written = 0;
static uint8_t sense_key = 0, sense_asc = 0, sense_ascq = 0;

volatile uint8_t usb_msc_last_op = 0;
volatile uint32_t usb_msc_commands = 0;

static volatile uint8_t attention = 0;

void usb_msc_set_media(const usb_msc_media_t *m)
{
	media = m;
	attention = m != NULL;                           // the computer is told a medium arrived
}
void usb_msc_clear_host_state(void) { host_seen = 0; ejected = 0; }
int usb_msc_host_seen(void) { return host_seen; }
int usb_msc_ejected(void) { return ejected; }
uint32_t usb_msc_sectors_read(void) { return n_read; }
uint32_t usb_msc_sectors_written(void) { return n_written; }
void usb_msc_reset_request(void) { need_init = 1; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void stall_in(void)  { *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT) |= USB_ENDPTCTRL_TXS; }
static void stall_out(void) { *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_RX_ENDPOINT) |= USB_ENDPTCTRL_RXS; }

static void queue_cbw(void)
{
	if (cbw_queued) return;
	cbw_xfer = &rx_xfer[rx_head];
	rx_head = (rx_head + 1) % RXN;
	NVIC_DISABLE_IRQ(IRQ_USB1);
	arm_dcache_delete(cbw_buf, sizeof cbw_buf);
	usb_prepare_transfer(cbw_xfer, cbw_buf, sizeof cbw_buf, 0);
	usb_receive(MSC_RX_ENDPOINT, cbw_xfer);
	NVIC_ENABLE_IRQ(IRQ_USB1);
	cbw_queued = 1;
}

// Called by the USB core (in its interrupt) on SET_CONFIGURATION: the poll
// starts the protocol over (need_init), configuring the endpoints.
void usb_msc_configure(void)
{
	pkt = usb_high_speed ? MSC_RX_SIZE_480 : MSC_RX_SIZE_12;
	usb_config_tx(MSC_TX_ENDPOINT, pkt, 0, NULL);
	usb_config_rx(MSC_RX_ENDPOINT, pkt, 0, NULL);
	need_init = 1;
}

static void init_protocol(void)
{
	need_init = 0;
	reset_requested = 0;
	NVIC_DISABLE_IRQ(IRQ_USB1);
	usb_config_tx(MSC_TX_ENDPOINT, pkt, 0, NULL);     // (the endpoints' queues empty again)
	usb_config_rx(MSC_RX_ENDPOINT, pkt, 0, NULL);
	NVIC_ENABLE_IRQ(IRQ_USB1);
	memset(rx_xfer, 0, sizeof rx_xfer);
	memset(tx_xfer, 0, sizeof tx_xfer);
	async_used = 0;
	cbw_queued = 0;
	host_seen = 0;
	ejected = 0;
	sense_key = sense_asc = sense_ascq = 0;
	queue_cbw();
}

// Waits for a transfer; returns its status (bit 7 clear), or 0x40 (halted) when
// the host went away, reset us or took too long.
static uint32_t wait_done(transfer_t *x, uint32_t timeout_ms)
{
	uint32_t t0 = millis();
	while (1) {
		if (!usb_configuration || reset_requested || need_init) return 0x40;
		uint32_t st = usb_transfer_status(x);
		if (!(st & 0x80)) return st;
		if (millis() - t0 > timeout_ms) return 0x40;
	}
}

static int send_in(const void *buf, uint32_t len)
{
	if (async_mode && async_used >= 3) return 0;
	transfer_t *x = &tx_xfer[tx_head];
	tx_head = (tx_head + 1) % TXN;
	NVIC_DISABLE_IRQ(IRQ_USB1);
	if (len) arm_dcache_flush_delete((void *)buf, (len + 31) & ~31u);
	usb_prepare_transfer(x, buf, len, 0);
	usb_transmit(MSC_TX_ENDPOINT, x);
	NVIC_ENABLE_IRQ(IRQ_USB1);
	if (async_mode) {                               // queued; the poll sees it through
		async_xfer[async_used++] = x;
		return 1;
	}
	return !(wait_done(x, 8000) & 0x68);
}

// Receives up to len bytes (it ends on a short packet); *got = how many.
static int recv_out(void *buf, uint32_t len, uint32_t *got)
{
	transfer_t *x = &rx_xfer[rx_head];
	rx_head = (rx_head + 1) % RXN;
	NVIC_DISABLE_IRQ(IRQ_USB1);
	arm_dcache_delete(buf, (len + 31) & ~31u);
	usb_prepare_transfer(x, buf, len, 0);
	usb_receive(MSC_RX_ENDPOINT, x);
	NVIC_ENABLE_IRQ(IRQ_USB1);
	uint32_t st = wait_done(x, 8000);
	if (st & 0x68) return 0;
	*got = len - ((st >> 16) & 0x7FFF);
	return 1;
}

// ---- One command

typedef struct {
	uint32_t tag, dlen, residue;
	uint8_t in, status;
} cmd_t;

static void fail(cmd_t *c, uint8_t key, uint8_t asc, uint8_t ascq)
{
	sense_key = key;
	sense_asc = asc;
	sense_ascq = ascq;
	c->status = 1;
	if (c->dlen) {                                  // the data stage the host expects isn't coming (or going)
		if (c->in) stall_in(); else stall_out();
	}
}

// Data to the host: len bytes, at most what it asked for.
static void data_in(cmd_t *c, const void *buf, uint32_t len)
{
	if (len > c->dlen) len = c->dlen;
	if (!len) {
		if (c->dlen) fail(c, 5, 0x24, 0);
		return;
	}
	if (!send_in(buf, len)) { c->status = 2; return; }
	if (len < c->dlen && len % pkt == 0) send_in(buf, 0);   // (a zero-length packet ends it)
	c->residue = c->dlen - len;
}

static uint32_t sector_count(void) { return media ? media->sector_count() : 0; }

static void do_read(cmd_t *c, uint32_t lba, uint32_t count)
{
	if ((uint64_t)lba + count > sector_count()) { fail(c, 5, 0x21, 0); return; }
	uint32_t sent = 0;
	while (count) {
		uint32_t n = count > DATA_BYTES / SECTOR ? DATA_BYTES / SECTOR : count;
		if (!media->read(lba, data_buf, n)) {
			sense_key = 3; sense_asc = 0x11; sense_ascq = 0;          // unrecovered read error
			c->status = 1;
			stall_in();
			c->residue = c->dlen > sent ? c->dlen - sent : 0;
			return;
		}
		if (!send_in(data_buf, n * SECTOR)) { c->status = 2; return; }
		sent += n * SECTOR;
		n_read += n;
		lba += n;
		count -= n;
	}
	if (sent < c->dlen && sent % pkt == 0) send_in(data_buf, 0);
	c->residue = c->dlen > sent ? c->dlen - sent : 0;
}

static void do_write(cmd_t *c, uint32_t lba, uint32_t count)
{
	if ((uint64_t)lba + count > sector_count()) { fail(c, 5, 0x21, 0); return; }
	uint32_t got_total = 0;
	while (count) {
		uint32_t n = count > DATA_BYTES / SECTOR ? DATA_BYTES / SECTOR : count, got = 0;
		if (!recv_out(data_buf, n * SECTOR, &got)) { c->status = 2; return; }
		if (got != n * SECTOR) { c->status = 2; return; }
		if (!media->write(lba, data_buf, n)) {
			sense_key = 3; sense_asc = 0x0C; sense_ascq = 0;          // write error
			c->status = 1;
			stall_out();
			c->residue = c->dlen > got_total ? c->dlen - got_total : 0;
			return;
		}
		got_total += got;
		n_written += n;
		lba += n;
		count -= n;
	}
	c->residue = c->dlen > got_total ? c->dlen - got_total : 0;
}

static void handle_command(cmd_t *c, const uint8_t *cb)
{
	// Without media (not in drive mode) it answers as an empty card reader:
	// the computer's probing is served, nothing mounts. INQUIRY and REQUEST
	// SENSE always work; a new medium is announced once (unit attention).
	if (cb[0] != 0x12 && cb[0] != 0x03) {
		if (!media) { fail(c, 2, 0x3A, 0); return; }                 // not ready, medium not present
		if (attention) { attention = 0; fail(c, 6, 0x28, 0); return; }  // unit attention: medium may have changed
	}
	switch (cb[0]) {
	case 0x00: // TEST UNIT READY
		break;
	case 0x03: { // REQUEST SENSE
		memset(small_buf, 0, 18);
		small_buf[0] = 0x70;
		small_buf[2] = sense_key;
		small_buf[7] = 10;
		small_buf[12] = sense_asc;
		small_buf[13] = sense_ascq;
		sense_key = sense_asc = sense_ascq = 0;
		data_in(c, small_buf, cb[4] < 18 ? cb[4] : 18);
		break;
	}
	case 0x12: { // INQUIRY
		if (cb[1] & 1) { fail(c, 5, 0x24, 0); break; }  // (no vital product data pages)
		memset(small_buf, 0, 36);
		small_buf[1] = 0x80;                          // removable
		small_buf[2] = 0x04;                          // SPC-2
		small_buf[3] = 0x02;
		small_buf[4] = 31;
		memcpy(small_buf + 8, "L7      ", 8);
		memcpy(small_buf + 16, "SD CARD         ", 16);
		memcpy(small_buf + 32, "1.0 ", 4);
		data_in(c, small_buf, cb[4] < 36 ? cb[4] : 36);
		break;
	}
	case 0x1A: // MODE SENSE (6): no pages, not write protected
		memset(small_buf, 0, 4);
		small_buf[0] = 3;
		data_in(c, small_buf, 4);
		break;
	case 0x5A: // MODE SENSE (10)
		memset(small_buf, 0, 8);
		small_buf[1] = 6;
		data_in(c, small_buf, 8);
		break;
	case 0x1B: // START STOP UNIT: eject
		if ((cb[4] & 2) && !(cb[4] & 1)) {
			if (media->sync) media->sync();
			ejected = 1;
		}
		break;
	case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
		break;
	case 0x23: // READ FORMAT CAPACITIES
		memset(small_buf, 0, 12);
		small_buf[3] = 8;
		put_be32(small_buf + 4, sector_count());
		small_buf[8] = 2;                             // formatted media
		small_buf[10] = SECTOR >> 8;
		data_in(c, small_buf, 12);
		break;
	case 0x25: // READ CAPACITY (10)
		put_be32(small_buf, sector_count() - 1);
		put_be32(small_buf + 4, SECTOR);
		data_in(c, small_buf, 8);
		break;
	case 0x28: // READ (10)
		do_read(c, be32(cb + 2), cb[7] << 8 | cb[8]);
		break;
	case 0x2A: // WRITE (10)
		do_write(c, be32(cb + 2), cb[7] << 8 | cb[8]);
		break;
	case 0x2F: // VERIFY (10)
		break;
	case 0x35: // SYNCHRONIZE CACHE (10)
		if (media->sync) media->sync();
		break;
	default:
		fail(c, 5, 0x20, 0);                          // invalid command operation code
		break;
	}
}

// What a command's answer needs: its data (queued by the handler) and then the
// status wrapper.
static void run_command(void)
{
	uint32_t st = usb_transfer_status(cbw_xfer);
	cbw_queued = 0;
	uint32_t got = sizeof cbw_buf - ((st >> 16) & 0x7FFF);
	host_seen = 1;
	if ((st & 0x68) || got != 31 || le32(cbw_buf) != 0x43425355) {   // not a valid command block: stall until reset
		stall_in();
		stall_out();
		queue_cbw();
		return;
	}
	cmd_t c;
	c.tag = le32(cbw_buf + 4);
	c.dlen = le32(cbw_buf + 8);
	c.in = cbw_buf[12] & 0x80;
	c.residue = c.dlen;
	c.status = 0;
	usb_msc_last_op = cbw_buf[15];
	usb_msc_commands++;
	handle_command(&c, cbw_buf + 15);
	if (reset_requested || need_init) return;
	if (c.status != 2) {                              // (2: the data stage broke: a reset follows)
		put_le32(csw_buf, 0x53425355);
		put_le32(csw_buf + 4, c.tag);
		put_le32(csw_buf + 8, c.residue);
		csw_buf[12] = c.status;
		send_in(csw_buf, 13);
	}
	if (!async_mode && !cbw_queued) queue_cbw();
}

// No medium: answers are queued, never waited for. Safe from an interrupt.
static void poll_async(void)
{
	if (!usb_configuration) { cbw_queued = 0; async_used = 0; return; }
	if (need_init || reset_requested) { init_protocol(); async_since = millis(); return; }
	if (async_used) {                                 // an answer on its way: done when all of it has gone
		int busy = 0;
		for (int k = 0; k < async_used; k++) busy |= (usb_transfer_status(async_xfer[k]) & 0x80) != 0;
		if (busy) {
			if (millis() - async_since > 3000) need_init = 1;   // (the host stopped reading: start over)
			return;
		}
		async_used = 0;
		queue_cbw();
		return;
	}
	if (!cbw_queued) queue_cbw();
	if (usb_transfer_status(cbw_xfer) & 0x80) return;  // nothing yet
	async_mode = 1;
	async_used = 0;
	run_command();
	async_mode = 0;
	if (!async_used && !cbw_queued) queue_cbw();
	async_since = millis();
}

// A medium: reads and writes wait for the card and the USB transfers.
static void poll_blocking(void)
{
	if (!usb_configuration) { cbw_queued = 0; return; }
	if (need_init || reset_requested) { init_protocol(); return; }
	if (async_used) {                                 // (an answer from before the medium came: let it finish)
		poll_async();
		return;
	}
	if (!cbw_queued) queue_cbw();
	if (usb_transfer_status(cbw_xfer) & 0x80) return;  // nothing yet
	run_command();
}

void usb_msc_poll(void)
{
	if (media) poll_blocking(); else poll_async();
}

#endif // MSC_INTERFACE
