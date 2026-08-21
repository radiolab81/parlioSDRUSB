# parlioSDRUSB
Baseband/RF over USB2.0 - powered by ESP32-P4 PARLIO/Parallel IO Interface (DMA - driven).
This is the port of the parlioSDR project (https://github.com/radiolab81/parlioSDR) from Ethernet to USB 2.0. This step allowed us to nearly double the transmission bandwidth when using good USB cables and controllers. So applications can send data with the next higher bit resolution or sampling rate.
Further speed improvements through optimizations, newer USB libraries or updated silicon versions of the ESP32-P4 would be possible.

### How to build?

Like parlioSDR, this project was created with ESP-IDF 6.1. The existing commands should therefore do the job.

```console
idf.py build

idf.py -p YOUR_PORT flash

idf.py -p YOUR_PORT monitor
```
Immediately after starting, parlioSDRUSB shows initialisation of usb in serial console:

```console
I (2081) tusb_desc: 
┌─────────────────────────────────┐
│  USB Device Descriptor Summary  │
├───────────────────┬─────────────┤
│bDeviceClass       │ 255         │
├───────────────────┼─────────────┤
│bDeviceSubClass    │ 0           │
├───────────────────┼─────────────┤
│bDeviceProtocol    │ 0           │
├───────────────────┼─────────────┤
│bMaxPacketSize0    │ 64          │
├───────────────────┼─────────────┤
│idVendor           │ 0x303a      │
├───────────────────┼─────────────┤
│idProduct          │ 0x4000      │
├───────────────────┼─────────────┤
│bcdDevice          │ 0x100       │
├───────────────────┼─────────────┤
│iManufacturer      │ 0x1         │
├───────────────────┼─────────────┤
│iProduct           │ 0x2         │
├───────────────────┼─────────────┤
│iSerialNumber      │ 0x3         │
├───────────────────┼─────────────┤
│bNumConfigurations │ 0x1         │
└───────────────────┴─────────────┘
I (2241) TinyUSB: TinyUSB Driver installed
I (2241) PARLIOSDR_USB: PARLIO: 5.00 MSPS, 8 Bit
I (2251) PARLIOSDR_USB: USB High-Speed SDR gestartet (Bulk Endpoint 0x01)
```

Please select the correct USB port for USB2.0 / 480 MBit on your board (usually this is only one USB port - on the Waveshare ESP32-P4 MODULE DEV-KIT it is the lower left USB port directly next to the Ethernet port). When using a USB-A to USB-A cable, and depending on the design of the development kit, the possibility of USB voltage backfeed must be considered. Therefore, it is strongly recommended to use a DC blocker or USB isolator. These should, of course, be suitable for data rates up to 480 Mbit/s.

On the PC side, the parlioSDRUSB shows:

```console
lsusb
...
Bus 009 Device 008: ID 303a:4000 RADIOLAB81 ESP32-PARLIOSDR-HS
...
```
To enable applications such as GNURadio, AMWaveSynth, COHIRADIA Streamer, etc. to continue to use TCP/IP, we have implemented a TCP-to-USB bridge.

To build the bridge, you need to install libusb and run these commands:

```console
sudo apt install libusb-1.0-0-dev
gcc bridge.c -o usb_bridge -lusb-1.0 -lpthread
```
Start the TCP2USB bridge (possibly with root privileges) with :

```console
sudo ./usb_bridge 
USB: ESP32-P4 High-Speed verbunden.
DATA: Warte auf SDR-Software auf Port 1234...
Bridge läuft. Daten: TCP 1234 -> USB Bulk. Control: TCP 5000 -> USB Control.
```
This program will redirect or tunnel both, the RF data port 1234 and the control port 5000 (for setting the bit width and sample rate) to USB.

## ⚠️ Chip Revision & Build Compatibility Notice

This firmware was built several months ago against **ESP32-P4 Engineering Sample**
silicon. The included `sdkconfig` pins the build to early chip revisions.

Since then, several newer mass-production ESP32-P4 revisions (up to v3.x) have
shipped. Revisions ≥ v3.0 are **not binary-compatible** with this build (over 50
hardware/register-level changes across ISP, CPU, L2MEM, MSPI, security modules).
A HEX image built for one side of this boundary will not boot on the other.

**If your board uses a recent chip revision, please rebuild from source**
with ESP-IDF 6.1 (which this project already uses — see `idf.py build` above)
rather than flashing a pre-built binary, and verify/adjust the chip revision range
and CPU frequency via `idf.py menuconfig` → **Component config → Hardware Settings**
and **→ ESP System Settings** for your actual hardware.

This also applies to the TCP↔USB `bridge.c` throughput path: since this project is
explicitly optimized for maximum USB 2.0 HS bandwidth, any future silicon or USB
library improvements (as noted in the project description) should be re-validated
against your specific board revision before relying on peak data rates.
