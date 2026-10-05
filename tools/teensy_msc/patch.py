#!/usr/bin/env python3
"""Adds a "Serial + Mass Storage" USB type to the Teensyduino 1.62 core (Teensy 4.0),
so the M8 can show its SD card to a computer as a disk (USB drive mode, usb_drive.cpp).

  python3 tools/teensy_msc/patch.py            # apply (safe to repeat)
  python3 tools/teensy_msc/patch.py --undo     # put the originals back

Everything new is behind the new USB type (build flag -DUSB_SERIAL_MSC, Arduino IDE:
Tools > USB Type > "Serial + Mass Storage (L7 patch)", arduino-cli:
teensy:avr:teensy40:usb=serialmsc); other USB types, and other boards,
build exactly as before. Edited files are first copied to <name>.l7orig. A
Teensyduino update overwrites the patch: run it again.

What it changes, in <teensyduino>/hardware/avr/1.62.0:
  cores/teensy4/usb_msc.c, usb_msc.h   new: the bulk-only SCSI engine
  cores/teensy4/usb_desc.h             the new type's device and endpoint layout
  cores/teensy4/usb_desc.c             its interface descriptor (high and full speed)
  cores/teensy4/usb.c                  configure hook, GET MAX LUN, bulk-only reset, a clear-halt that resets the toggle
  boards.txt                           the menu entry (Teensy 4.0)
"""
import glob
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MARK = "L7_MSC"


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


MSC_TYPE = """#elif defined(USB_SERIAL_MSC) // L7_MSC: Serial + a mass-storage disk (tools/teensy_msc)
  #define VENDOR_ID		0x16C0
  #define PRODUCT_ID		0x0483
  #define MANUFACTURER_NAME	{'T','e','e','n','s','y','d','u','i','n','o'}
  #define MANUFACTURER_NAME_LEN	11
  #define PRODUCT_NAME		{'U','S','B',' ','S','e','r','i','a','l'}
  #define PRODUCT_NAME_LEN	10
  #define EP0_SIZE		64
  #define NUM_ENDPOINTS         4
  #define NUM_INTERFACE		3
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
  #define MSC_INTERFACE		2	// mass storage
  #define MSC_TX_ENDPOINT	4
  #define MSC_RX_ENDPOINT	4
  #define MSC_TX_SIZE_12	64
  #define MSC_TX_SIZE_480	512
  #define MSC_RX_SIZE_12	64
  #define MSC_RX_SIZE_480	512
  #define ENDPOINT2_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT3_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT4_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK

#elif defined(USB_AUDIO)
"""


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


def apply():
    plat = find_platform()
    core = os.path.join(plat, "cores", "teensy4")
    for name in ("usb_msc.c", "usb_msc.h"):
        shutil.copy2(os.path.join(HERE, name), os.path.join(core, name))
    changed = []
    if edit(os.path.join(core, "usb_desc.h"), [("#elif defined(USB_AUDIO)\n", MSC_TYPE)]):
        changed.append("usb_desc.h")
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
                      "teensy40.menu.usb.serialmsc=Serial + Mass Storage (L7 patch)\n"
                      "teensy40.menu.usb.serialmsc.build.usbtype=USB_SERIAL_MSC\n"
                      "teensy40.menu.usb.serialmsc.upload_port.usbtype=USB_SERIAL\n", 1)
        open(boards, "w").write(b)
        changed.append("boards.txt")
    print("patched:", ", ".join(changed) if changed else "nothing to do (already patched)", "-", plat)


def undo():
    plat = find_platform()
    core = os.path.join(plat, "cores", "teensy4")
    for rel in ("cores/teensy4/usb_desc.h", "cores/teensy4/usb_desc.c", "cores/teensy4/usb.c", "boards.txt"):
        path = os.path.join(plat, rel)
        if os.path.exists(path + ".l7orig"):
            shutil.move(path + ".l7orig", path)
            print("restored", rel)
    for name in ("usb_msc.c", "usb_msc.h"):
        if os.path.exists(os.path.join(core, name)):
            os.remove(os.path.join(core, name))
            print("removed", name)


if __name__ == "__main__":
    undo() if "--undo" in sys.argv else apply()
