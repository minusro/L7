#!/usr/bin/env python3
"""Adds a "Serial + MIDI + Audio + Mass Storage" USB type to the Teensyduino 1.62 core (Teensy 4.0),
so the M8 can show its SD card to a computer as a disk (USB drive mode, usb_drive.cpp),
send and receive MIDI clock over USB (midi_usb.cpp), and audio: L7's own USB Audio Class 2.0
(usb_audio_l7.cpp: 16 channels to the computer, the main mix and each track; 2 from it).

  python3 tools/teensy_msc/patch.py            # apply (safe to repeat)
  python3 tools/teensy_msc/patch.py --undo     # put the originals back

Everything new is behind the new USB type (build flag -DUSB_SERIAL_MSC, Arduino IDE:
Tools > USB Type > "Serial + Mass Storage (L7 patch)", arduino-cli:
teensy:avr:teensy40:usb=serialmsc); other USB types, and other boards,
build exactly as before. Edited files are first copied to <name>.l7orig. A
Teensyduino update overwrites the patch: run it again.

What it changes, in <teensyduino>/hardware/avr/1.62.0:
  cores/teensy4/usb_msc.c, usb_msc.h   new: the bulk-only SCSI engine
  cores/teensy4/usb_audio_l7.cpp, .h   new: the UAC2 streams (rings, the feedback, the clock's requests)
  cores/teensy4/usb_desc.h             the new type's device and endpoint layout
  cores/teensy4/usb_desc.c             its mass-storage and UAC2 descriptors (high and full speed), the channels' names
  cores/teensy4/usb.c                  configure hook, GET MAX LUN, bulk-only reset, a clear-halt that resets the toggle;
                                       UAC2's requests (alternate settings, GET CUR / RANGE) to usb_audio_l7.cpp
  cores/teensy4/usb_audio.h, .cpp      Teensyduino's USB audio left out where AUDIO_UAC2 is defined
  boards.txt                           the menu entry (Teensy 4.0)
  libraries/SdFat/src/common/FsStructs.h, FsName.cpp  file names may contain a carriage return (the "Icon\\r" file
                                       of a macOS folder icon, ableton.cpp); SdFat refuses control characters.
                                       Stored as U+F00D, as macOS does (it maps control characters to U+F000+c)
"""
import glob
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MARK = "L7_MSC"
CORE_FILES = ("usb_msc.c", "usb_msc.h", "usb_audio_l7.cpp", "usb_audio_l7.h")


def find_platform():
    hits = sorted(glob.glob(os.path.expanduser("~/Library/Arduino15/packages/teensy/hardware/avr/*/cores/teensy4")))
    if not hits:
        sys.exit("no Teensyduino (teensy4 core) found under ~/Library/Arduino15")
    return os.path.dirname(os.path.dirname(hits[-1]))


def backup(path):
    if not os.path.exists(path + ".l7orig"):
        shutil.copy2(path, path + ".l7orig")


def edit(path, steps):
    s = open(path).read()
    if MARK in s:
        return False
    backup(path)
    for old, new in steps:
        if s.count(old) < 1:
            sys.exit("anchor not found in %s:\n%s" % (path, old))
        s = s.replace(old, new, 1)
    open(path, "w").write(s)
    return True


MIDI_MARK = "L7_UAC2"
MSC_TYPE = """#elif defined(USB_SERIAL_MSC) // L7_MSC L7_MIDI L7_UAC2: Serial + MIDI + Audio + a mass-storage disk (tools/teensy_msc)
  #define VENDOR_ID		0x16C0
  #define PRODUCT_ID		0x0483
  #define MANUFACTURER_NAME	{'T','e','e','n','s','y','d','u','i','n','o'}
  #define MANUFACTURER_NAME_LEN	11
  #define PRODUCT_NAME		{'U','S','B',' ','S','e','r','i','a','l'}
  #define PRODUCT_NAME_LEN	10
  #define EP0_SIZE		64
  #define NUM_ENDPOINTS         7
  #define NUM_INTERFACE		7
  #define CDC_IAD_DESCRIPTOR	1
  #define CDC_STATUS_INTERFACE	0
  #define CDC_DATA_INTERFACE	1	// Serial
  #define CDC_ACM_ENDPOINT	2
  #define CDC_RX_ENDPOINT       3
  #define CDC_TX_ENDPOINT       3
  #define CDC_ACM_SIZE          16
  #define CDC_RX_SIZE_480       512
  #define CDC_TX_SIZE_480       512
  #define CDC_RX_SIZE_12        64
  #define CDC_TX_SIZE_12        64
  #define MIDI_INTERFACE        6	// MIDI (as Teensyduino's Serial + MIDI; numbered after the audio function's, as UAC2 wants)
  #define MIDI_NUM_CABLES       1
  #define MIDI_TX_ENDPOINT      4
  #define MIDI_TX_SIZE_12       64
  #define MIDI_TX_SIZE_480      512
  #define MIDI_RX_ENDPOINT      4
  #define MIDI_RX_SIZE_12       64
  #define MIDI_RX_SIZE_480      512
  #define MSC_INTERFACE		2	// mass storage
  #define MSC_TX_ENDPOINT	5
  #define MSC_RX_ENDPOINT	5
  #define MSC_TX_SIZE_12	64
  #define MSC_TX_SIZE_480	512
  #define MSC_RX_SIZE_12	64
  #define MSC_RX_SIZE_480	512
  #define ENDPOINT2_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT3_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT4_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define AUDIO_INTERFACE	3	// audio: USB Audio Class 2.0 (usb_audio_l7.cpp), 3 interfaces: control, to the computer, from it
  #define AUDIO_UAC2		1
  #define AUDIO_CLOCK_ID	9
  #define AUDIO_TX_CHANNELS_480	16	// to the computer: the master and the 7 tracks (top row first), stereo pairs
  #define AUDIO_TX_CHANNELS_12	2	// (full speed: the main mix only)
  #define AUDIO_RX_CHANNELS	2	// from the computer
  #define AUDIO_TX_ENDPOINT     6
  #define AUDIO_TX_SIZE_480     (7*2*AUDIO_TX_CHANNELS_480)	// up to 7 frames a microframe (44.1 kHz: 5.5)
  #define AUDIO_TX_SIZE_12      (47*2*AUDIO_TX_CHANNELS_12)	// up to 47 frames a frame
  #define AUDIO_RX_ENDPOINT     6
  #define AUDIO_RX_SIZE_480     (7*2*AUDIO_RX_CHANNELS)
  #define AUDIO_RX_SIZE_12      (47*2*AUDIO_RX_CHANNELS)
  #define AUDIO_SYNC_ENDPOINT	7
  #define AUDIO_CHANNEL_NAMES	0x10	// string descriptors 0x10-0x1F: the channels to the computer
  #define ENDPOINT5_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT6_CONFIG	ENDPOINT_RECEIVE_ISOCHRONOUS + ENDPOINT_TRANSMIT_ISOCHRONOUS
  #define ENDPOINT7_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_ISOCHRONOUS

#elif defined(USB_AUDIO)
"""

CHANNEL_NAMES = ["Master L", "Master R"] + ["Ch %d %s" % (t, side) for t in range(1, 8) for side in "LR"]   # (Ch 1: the top row)


def uac2_descriptor(speed):
    """The audio function (USB Audio Class 2.0) for one speed: 16 channels to the computer at
    high speed (2 at full speed), 2 from it; one fixed 44.1 kHz clock; no controls."""
    hs = speed == "480"
    tx_ch = "AUDIO_TX_CHANNELS_%s" % speed
    tx_config = "0,0,0,0" if hs else "0x03,0,0,0"           # (16: named, no positions; 2: front left, right)
    tx_names = "AUDIO_CHANNEL_NAMES" if hs else "0"
    interval = "1" if hs else "1"                            # data: every microframe (high speed) / frame
    fb_interval = "4" if hs else "1"                         # feedback: every 1 ms
    fb_size = "4" if hs else "3"
    return """#if defined(AUDIO_UAC2) // L7_UAC2: tools/teensy_msc/patch.py
        // interface association: the audio function, UAC2 (protocol 0x20)
        8, 11, AUDIO_INTERFACE, 3, 0x01, 0x00, 0x20, 0,
        // AudioControl interface
        9, 4, AUDIO_INTERFACE, 0, 0, 0x01, 0x01, 0x20, 0,
        // class-specific AC header: UAC 2.00, category I/O box, wTotalLength 75 (the AC descriptors), no controls
        9, 0x24, 0x01, 0x00, 0x02, 0x08, 75, 0, 0x00,
        // clock source: internal, fixed; frequency and validity readable
        8, 0x24, 0x0A, AUDIO_CLOCK_ID, 0x01, 0x05, 0, 0,
        // input terminal 1 (line connector): what goes to the computer
        17, 0x24, 0x02, 1, 0x03, 0x06, 0, AUDIO_CLOCK_ID, %(tx_ch)s, %(tx_config)s, %(tx_names)s, 0x00, 0x00, 0,
        // output terminal 2 (USB streaming): to the computer
        12, 0x24, 0x03, 2, 0x01, 0x01, 0, 1, AUDIO_CLOCK_ID, 0x00, 0x00, 0,
        // input terminal 3 (USB streaming): from the computer
        17, 0x24, 0x02, 3, 0x01, 0x01, 0, AUDIO_CLOCK_ID, AUDIO_RX_CHANNELS, 0x03, 0, 0, 0, 0, 0x00, 0x00, 0,
        // output terminal 4 (line connector): into L7
        12, 0x24, 0x03, 4, 0x03, 0x06, 0, 3, AUDIO_CLOCK_ID, 0x00, 0x00, 0,
        // AudioStreaming interface: to the computer, alternate 0 (idle)
        9, 4, AUDIO_INTERFACE+1, 0, 0, 0x01, 0x02, 0x20, 0,
        // alternate 1: streaming
        9, 4, AUDIO_INTERFACE+1, 1, 1, 0x01, 0x02, 0x20, 0,
        // AS general: terminal 2, PCM
        16, 0x24, 0x01, 2, 0x00, 0x01, 0x01, 0, 0, 0, %(tx_ch)s, %(tx_config)s, %(tx_names)s,
        // format type I: 2-byte subslots, 16 bits
        6, 0x24, 0x02, 0x01, 2, 16,
        // endpoint: isochronous, asynchronous, data (IN)
        7, 5, AUDIO_TX_ENDPOINT | 0x80, 0x05, LSB(AUDIO_TX_SIZE_%(speed)s), MSB(AUDIO_TX_SIZE_%(speed)s), %(interval)s,
        // class-specific endpoint
        8, 0x25, 0x01, 0x00, 0x00, 0, 0, 0,
        // AudioStreaming interface: from the computer, alternate 0 (idle)
        9, 4, AUDIO_INTERFACE+2, 0, 0, 0x01, 0x02, 0x20, 0,
        // alternate 1: streaming, with the feedback endpoint
        9, 4, AUDIO_INTERFACE+2, 1, 2, 0x01, 0x02, 0x20, 0,
        // AS general: terminal 3, PCM, stereo
        16, 0x24, 0x01, 3, 0x00, 0x01, 0x01, 0, 0, 0, AUDIO_RX_CHANNELS, 0x03, 0, 0, 0, 0,
        // format type I: 2-byte subslots, 16 bits
        6, 0x24, 0x02, 0x01, 2, 16,
        // endpoint: isochronous, asynchronous, data (OUT)
        7, 5, AUDIO_RX_ENDPOINT, 0x05, LSB(AUDIO_RX_SIZE_%(speed)s), MSB(AUDIO_RX_SIZE_%(speed)s), %(interval)s,
        // class-specific endpoint
        8, 0x25, 0x01, 0x00, 0x00, 0, 0, 0,
        // feedback endpoint (IN): the rate L7 wants
        7, 5, AUDIO_SYNC_ENDPOINT | 0x80, 0x11, %(fb_size)s, 0, %(fb_interval)s,
#else
""" % dict(tx_ch=tx_ch, tx_config=tx_config, tx_names=tx_names, interval=interval,
           fb_interval=fb_interval, fb_size=fb_size, speed=speed)


def msc_descriptor(speed):
    return """#ifdef MSC_INTERFACE // L7_MSC
        // interface descriptor: mass storage, SCSI transparent, bulk-only
        9,                                      // bLength
        4,                                      // bDescriptorType
        MSC_INTERFACE,                          // bInterfaceNumber
        0,                                      // bAlternateSetting
        2,                                      // bNumEndpoints
        0x08,                                   // bInterfaceClass (0x08 = mass storage)
        0x06,                                   // bInterfaceSubClass (SCSI)
        0x50,                                   // bInterfaceProtocol (bulk-only)
        0,                                      // iInterface
        7,                                      // bLength
        5,                                      // bDescriptorType
        MSC_TX_ENDPOINT | 0x80,                 // bEndpointAddress
        0x02,                                   // bmAttributes (0x02=bulk)
        LSB(MSC_TX_SIZE_%s),MSB(MSC_TX_SIZE_%s), // wMaxPacketSize
        0,                                      // bInterval
        7,                                      // bLength
        5,                                      // bDescriptorType
        MSC_RX_ENDPOINT,                        // bEndpointAddress
        0x02,                                   // bmAttributes (0x02=bulk)
        LSB(MSC_RX_SIZE_%s),MSB(MSC_RX_SIZE_%s), // wMaxPacketSize
        0,                                      // bInterval
#endif // MSC_INTERFACE

""" % (speed, speed, speed, speed)


USB_C_REQUESTS = """#endif
#if defined(MSC_INTERFACE) // L7_MSC
	  case 0xFEA1: // GET MAX LUN (mass storage class): one logical unit
		if (setup.wIndex == MSC_INTERFACE && setup.wLength >= 1) {
			endpoint0_buffer[0] = 0;
			endpoint0_transmit(endpoint0_buffer, 1, 0);
			return;
		}
		break;
	  case 0xFF21: // BULK-ONLY MASS STORAGE RESET
		if (setup.wIndex == MSC_INTERFACE) {
			usb_msc_reset_request();
			endpoint0_receive(NULL, 0, 0);
			return;
		}
		break;
#endif
	}
	printf("endpoint 0 stall\\n");"""


def delay_fields(desc):
    """The audio streams' bDelay (the delay they report, in ms): AUDIO_TX_DELAY_MS (to the
    computer) and AUDIO_RX_DELAY_MS (from it) when the USB type sets them, else 3 as before."""
    s = open(desc).read()
    if "L7_DELAY" in s:
        return False
    line = "\t3,\t\t\t\t\t// bDelay (approx 3ms delay, audio lib updates)\n"
    if s.count(line) != 4:
        sys.exit("usb_desc.c: the audio streams' bDelay lines not as expected")
    parts = s.split(line)
    out = parts[0]
    for k, rest in enumerate(parts[1:]):
        macro = "AUDIO_TX_DELAY_MS" if k % 2 == 0 else "AUDIO_RX_DELAY_MS"   # (each speed: to the computer, then from it)
        out += "\t%s,\t\t\t// bDelay  L7_DELAY\n" % macro + rest
    head = '#include "usb_desc.h"\n'
    if out.count(head) < 1:
        sys.exit("usb_desc.c: include not found")
    out = out.replace(head, head + "#ifndef AUDIO_TX_DELAY_MS // L7_DELAY\n#define AUDIO_TX_DELAY_MS 3\n#endif\n#ifndef AUDIO_RX_DELAY_MS\n#define AUDIO_RX_DELAY_MS 3\n#endif\n", 1)
    open(desc, "w").write(out)
    return True


USB_C_UAC2 = """#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2) // L7_UAC2: USB Audio Class 2.0 (usb_audio_l7.cpp)
	  case 0x0B01: // SET_INTERFACE (alternate setting)
		if (setup.wIndex == AUDIO_INTERFACE+1 || setup.wIndex == AUDIO_INTERFACE+2) {
			usb_audio_set_interface(setup.wIndex - AUDIO_INTERFACE, setup.wValue);
			endpoint0_receive(NULL, 0, 0);
			return;
		}
		break;
	  case 0x0A81: // GET_INTERFACE (alternate setting)
		if (setup.wIndex == AUDIO_INTERFACE+1 || setup.wIndex == AUDIO_INTERFACE+2) {
			endpoint0_buffer[0] = setup.wIndex == AUDIO_INTERFACE+1 ? usb_audio_transmit_setting : usb_audio_receive_setting;
			endpoint0_transmit(endpoint0_buffer, 1, 0);
			return;
		}
		break;
	  case 0x0121: // SET CUR / RANGE (to an interface or entity): taken, nothing to set (a fixed clock)
	  case 0x0221:
	  case 0x0122:
		if (setup.wLength <= sizeof(endpoint0_buffer)) {
			endpoint0_receive(endpoint0_buffer, setup.wLength, 0);
			return;
		}
		break;
	  case 0x01A1: // GET CUR
	  case 0x02A1: // GET RANGE
		{
			const uint8_t *reply;
			uint32_t len;
			if (usb_audio_class_request(setup.wRequestAndType, setup.wValue, setup.wIndex, &reply, &len)) {
				endpoint0_transmit(reply, len < setup.wLength ? len : setup.wLength, 0);
				return;
			}
		}
		break;
#elif defined(AUDIO_INTERFACE)
	  case 0x0B01: // SET_INTERFACE (alternate setting)"""


def edit_marked(path, marker, steps):
    """Like edit(), for a later addition: done once, recognised by its own marker."""
    s = open(path).read()
    if marker in s:
        return False
    backup(path)
    for old, new in steps:
        if s.count(old) < 1:
            sys.exit("anchor not found in %s:\n%s" % (path, old))
        s = s.replace(old, new, 1)
    open(path, "w").write(s)
    return True


def channel_names():
    """The channels' names as string descriptors (usb_desc.c), and their entries in the descriptor list."""
    names = "#ifdef AUDIO_CHANNEL_NAMES // L7_UAC2: the channels to the computer, by name\n"
    entries = "#ifdef AUDIO_CHANNEL_NAMES // L7_UAC2\n"
    for k, name in enumerate(CHANNEL_NAMES):
        names += "PROGMEM const struct usb_string_descriptor_struct usb_string_channel%d = {%d, 3, {%s}};\n" % (
            k, 2 + 2 * len(name), ",".join("'%s'" % c for c in name))
        entries += "        {0x0300 + AUDIO_CHANNEL_NAMES + %d, 0x0409, (const uint8_t *)&usb_string_channel%d, 0},\n" % (k, k)
    return names + "#endif\n\n", entries + "#endif\n"


def uac2_desc(desc):
    """usb_desc.c: the UAC2 audio function (both speeds), its size, the channels' names (renamed: refreshed)."""
    s = open(desc).read()
    if "L7_UAC2" in s:
        names, _ = channel_names()
        i = s.index("#ifdef AUDIO_CHANNEL_NAMES // L7_UAC2: the channels")
        j = s.index("#endif\n\n", i) + len("#endif\n\n")
        if s[i:j] == names:
            return False
        open(desc, "w").write(s[:i] + names + s[j:])
        return True
    backup(desc)
    size_old = "#define AUDIO_INTERFACE_DESC_SIZE\t8 + 9+10+12+9+12+10+9 + 9+9+7+11+9+7 + 9+9+7+11+9+7+9\n"
    if s.count(size_old) != 1:
        sys.exit("usb_desc.c: the audio size not as expected")
    s = s.replace(size_old, "#if defined(AUDIO_UAC2) // L7_UAC2\n#define AUDIO_INTERFACE_DESC_SIZE\t8+9+75 + 9+9+16+6+7+8 + 9+9+16+6+7+8+7\n#else\n"
                  + size_old + "#endif\n", 1)
    start, end = "#ifdef AUDIO_INTERFACE\n", "\t0,\t\t\t\t\t// bSynchAddress\n#endif\n\n#ifdef MULTITOUCH_INTERFACE"
    if s.count(start) != 2 or s.count(end) != 2:
        sys.exit("usb_desc.c: the audio descriptors not as expected")
    at = 0
    for speed in ("480", "12"):
        i = s.index(start, at) + len(start)
        s = s[:i] + uac2_descriptor(speed) + s[i:]
        j = s.index(end, i) + len("\t0,\t\t\t\t\t// bSynchAddress\n")
        s = s[:j] + "#endif // L7_UAC2\n" + s[j:]
        at = j
    names, entries = channel_names()
    list_head = "const usb_descriptor_list_t usb_descriptor_list[] = {\n"
    string0 = "        {0x0300, 0x0000, (const uint8_t *)&string0, 0},\n"
    if s.count(list_head) != 1 or s.count(string0) != 1:
        sys.exit("usb_desc.c: the descriptor list not as expected")
    s = s.replace(list_head, names + list_head, 1).replace(string0, entries + string0, 1)
    open(desc, "w").write(s)
    return True


def apply():
    plat = find_platform()
    core = os.path.join(plat, "cores", "teensy4")
    for name in CORE_FILES:
        shutil.copy2(os.path.join(HERE, name), os.path.join(core, name))
    changed = []
    hdr = os.path.join(core, "usb_desc.h")
    if edit(hdr, [("#elif defined(USB_AUDIO)\n", MSC_TYPE)]):
        changed.append("usb_desc.h")
    else:                                            # patched before MIDI and audio were in it: its block replaced
        h = open(hdr).read()
        if MIDI_MARK not in h:
            i = h.index("#elif defined(USB_SERIAL_MSC)")
            j = h.index("#elif defined(USB_AUDIO)\n", i) + len("#elif defined(USB_AUDIO)\n")
            open(hdr, "w").write(h[:i] + MSC_TYPE + h[j:])
            changed.append("usb_desc.h (MIDI added)")
    desc = os.path.join(core, "usb_desc.c")
    s = open(desc).read()
    if MARK not in s:
        backup(desc)
        old_pos = "#define KEYMEDIA_INTERFACE_DESC_POS\tMTP_INTERFACE_DESC_POS+MTP_INTERFACE_DESC_SIZE\n"
        new_pos = ("#define MSC_INTERFACE_DESC_POS\t\tMTP_INTERFACE_DESC_POS+MTP_INTERFACE_DESC_SIZE  // L7_MSC\n"
                   "#ifdef  MSC_INTERFACE\n#define MSC_INTERFACE_DESC_SIZE\t\t9+7+7\n#else\n#define MSC_INTERFACE_DESC_SIZE\t0\n#endif\n\n"
                   "#define KEYMEDIA_INTERFACE_DESC_POS\tMSC_INTERFACE_DESC_POS+MSC_INTERFACE_DESC_SIZE\n")
        if s.count(old_pos) != 1 or s.count("#endif // MTP_INTERFACE\n") != 2:
            sys.exit("usb_desc.c: anchors not as expected")
        s = s.replace(old_pos, new_pos, 1)
        marker = "#endif // MTP_INTERFACE\n"
        i = s.index(marker) + len(marker)
        s = s[:i] + "\n" + msc_descriptor("480") + s[i:]
        j = s.index(marker, i + len(msc_descriptor("480"))) + len(marker)
        s = s[:j] + "\n" + msc_descriptor("12") + s[j:]
        open(desc, "w").write(s)
        changed.append("usb_desc.c")
    if delay_fields(desc):
        changed.append("usb_desc.c (bDelay)")
    if uac2_desc(desc):
        changed.append("usb_desc.c (UAC2)")
    usbc = os.path.join(core, "usb.c")
    mtp_end = ("\t\tbreak;\n#endif\n\t}\n\tprintf(\"endpoint 0 stall\\n\");")
    if edit(usbc, [
        ('#include "usb_mtp.h"\n', '#include "usb_mtp.h"\n#include "usb_msc.h" // L7_MSC\n'),
        ("\t\t#if defined(MTP_INTERFACE)\n\t\tusb_mtp_configure();\n\t\t#endif\n",
         "\t\t#if defined(MTP_INTERFACE)\n\t\tusb_mtp_configure();\n\t\t#endif\n\t\t#if defined(MSC_INTERFACE)\n\t\tusb_msc_configure();\n\t\t#endif\n"),
        (mtp_end, "\t\tbreak;\n" + USB_C_REQUESTS),
        # CLEAR_FEATURE(ENDPOINT_HALT) must also reset the endpoint's data toggle (the mass-storage
        # engine stalls endpoints for commands it doesn't support).
        ("\t\t\t*((volatile uint32_t *)&USB1_ENDPTCTRL0 + endpoint) &= ~USB_ENDPTCTRL_RXS;\n\t\t}\n\t\tendpoint0_receive(NULL, 0, 0);\n\t\treturn;\n#ifdef EXPERIMENTAL_INTERFACE",
         "\t\t\t*((volatile uint32_t *)&USB1_ENDPTCTRL0 + endpoint) &= ~USB_ENDPTCTRL_RXS;\n\t\t}\n"
         "#if defined(MSC_INTERFACE) // L7_MSC\n"
         "\t\tif (endpoint == MSC_TX_ENDPOINT) *((volatile uint32_t *)&USB1_ENDPTCTRL0 + endpoint) |= dir ? USB_ENDPTCTRL_TXR : USB_ENDPTCTRL_RXR;\n"
         "#endif\n"
         "\t\tendpoint0_receive(NULL, 0, 0);\n\t\treturn;\n#ifdef EXPERIMENTAL_INTERFACE"),
        ("uint32_t usb_transfer_status(const transfer_t *transfer)\n{\n#if defined(MTP_INTERFACE)",
         "uint32_t usb_transfer_status(const transfer_t *transfer)\n{\n#if defined(MTP_INTERFACE) || defined(MSC_INTERFACE)"),
    ]):
        changed.append("usb.c")
    if edit_marked(usbc, "L7_UAC2", [
        ("#if defined(AUDIO_INTERFACE)\n\t  case 0x0B01: // SET_INTERFACE (alternate setting)", USB_C_UAC2),
        ("#ifdef AUDIO_INTERFACE\n\tif (setup.word1 == 0x02010121",
         "#if defined(AUDIO_INTERFACE) && !defined(AUDIO_UAC2) // L7_UAC2\n\tif (setup.word1 == 0x02010121"),
    ]):
        changed.append("usb.c (UAC2)")
    # Teensyduino's USB audio (UAC1) left out where L7's is used
    if edit_marked(os.path.join(core, "usb_audio.h"), "L7_UAC2", [
        ('#include "usb_desc.h"\n#ifdef AUDIO_INTERFACE\n',
         '#include "usb_desc.h"\n#include "usb_audio_l7.h" // L7_UAC2\n#if defined(AUDIO_INTERFACE) && !defined(AUDIO_UAC2)\n'),
    ]):
        changed.append("usb_audio.h")
    if edit_marked(os.path.join(core, "usb_audio.cpp"), "L7_UAC2", [
        ('#ifdef AUDIO_INTERFACE\n', '#if defined(AUDIO_INTERFACE) && !defined(AUDIO_UAC2) // L7_UAC2: usb_audio_l7.cpp instead\n'),
    ]):
        changed.append("usb_audio.cpp")
    if edit(os.path.join(plat, "libraries", "SdFat", "src", "common", "FsStructs.h"),
            [("  return c < 0X20 || c == '\"'", "  return (c < 0X20 && c != 0X0D) /* L7_MSC: a CR is fine */ || c == '\"'")]):
        changed.append("FsStructs.h")
    if edit(os.path.join(plat, "libraries", "SdFat", "src", "common", "FsName.cpp"),
            [("    if (cp <= 0XFFFF) {\n      rtn = cp;", "    if (cp <= 0XFFFF) {  // L7_MSC: a CR is stored as macOS does, U+F00D\n      rtn = cp == 0X0D ? 0XF00D : cp;")]):
        changed.append("FsName.cpp")
    boards = os.path.join(plat, "boards.txt")
    b = open(boards).read()
    if MARK not in b:
        backup(boards)
        anchor = ("teensy40.menu.usb.serialmtp=Serial + MTP Disk (Experimental)\n"
                  "teensy40.menu.usb.serialmtp.build.usbtype=USB_MTPDISK_SERIAL\n"
                  "teensy40.menu.usb.serialmtp.upload_port.usbtype=USB_MTPDISK_SERIAL\n")
        if anchor not in b:
            sys.exit("boards.txt: anchor not found")
        b = b.replace(anchor, anchor +
                      "# L7_MSC: tools/teensy_msc/patch.py\n"
                      "teensy40.menu.usb.serialmsc=Serial + MIDI + Audio + Mass Storage (L7 patch)\n"
                      "teensy40.menu.usb.serialmsc.build.usbtype=USB_SERIAL_MSC\n"
                      "teensy40.menu.usb.serialmsc.upload_port.usbtype=USB_SERIAL\n", 1)
        open(boards, "w").write(b)
        changed.append("boards.txt")
    else:                                            # (its name from before MIDI and audio)
        for old in ("serialmsc=Serial + Mass Storage (L7 patch)", "serialmsc=Serial + MIDI + Mass Storage (L7 patch)"):
            if old in b:
                open(boards, "w").write(b.replace(old, "serialmsc=Serial + MIDI + Audio + Mass Storage (L7 patch)"))
                changed.append("boards.txt (name)")
    print("patched:", ", ".join(changed) if changed else "nothing to do (already patched)", "-", plat)


def undo():
    plat = find_platform()
    core = os.path.join(plat, "cores", "teensy4")
    for rel in ("cores/teensy4/usb_desc.h", "cores/teensy4/usb_desc.c", "cores/teensy4/usb.c", "boards.txt",
                "cores/teensy4/usb_audio.h", "cores/teensy4/usb_audio.cpp",
                "libraries/SdFat/src/common/FsStructs.h",
                "libraries/SdFat/src/common/FsName.cpp"):
        path = os.path.join(plat, rel)
        if os.path.exists(path + ".l7orig"):
            shutil.move(path + ".l7orig", path)
            print("restored", rel)
    for name in CORE_FILES:
        if os.path.exists(os.path.join(core, name)):
            os.remove(os.path.join(core, name))
            print("removed", name)


if __name__ == "__main__":
    undo() if "--undo" in sys.argv else apply()
