# Infinite Noise TRNG Kernel Module

A Linux kernel module for the Infinite Noise TRNG USB device. This driver directly interfaces with the FT240X-based hardware, implements health monitoring, Keccak-1600 whitening, and registers as an hwrng device to feed entropy to the kernel's random subsystem.

## Features

- Hardware random number generator (hwrng) interface at `/dev/hwrng`
- Automatic entropy feeding to kernel CRNG (no rngd required)
- Character device interface at `/dev/infnoiseX`
- Real-time health monitoring with entropy validation
- Keccak-1600 sponge for cryptographic whitening
- Configurable output multiplier for higher throughput
- Hot-plug support
- No conflicts with ftdi_sio driver

## Quick Start

### 1. Reprogram Device VID/PID (One-Time Setup)

The kernel module only recognizes devices with the Infinite Noise VID/PID (`1209:3701`). New devices ship with the default FTDI VID/PID (`0403:6015`) which must be reprogrammed.

See [Device Setup](#device-setup) for detailed instructions.

### 2. Build and Load Module

```bash
# Install prerequisites (Debian/Ubuntu)
sudo apt install linux-headers-$(uname -r) build-essential

# Build
cd software/kernel
make

# Load
sudo insmod infnoise.ko

# Verify
dmesg | grep infnoise
```

### 3. Use

```bash
# Read random data
sudo dd if=/dev/hwrng bs=64 count=10 | xxd
```

## Device Setup

### Why Reprogram?

The Infinite Noise TRNG uses an FTDI FT240X chip which ships with FTDI's default VID/PID (`0403:6015`). This causes conflicts with the kernel's `ftdi_sio` serial driver. By reprogramming to the registered Infinite Noise VID/PID (`1209:3701`), the device is uniquely identified and automatically bound to this driver.

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt install libftdi1-dev ftdi-eeprom

# Fedora
sudo dnf install libftdi-devel ftdi-eeprom

# Arch
sudo pacman -S libftdi
```

### Check Current VID/PID

```bash
lsusb | grep -i "ftdi\|1209"
```

If you see `0403:6015`, the device needs reprogramming. If you see `1209:3701`, it's already configured.

### Program the EEPROM

An `infnoise-eeprom.conf` file is included in this repository.

**Warning:** This permanently modifies the device's EEPROM. Double-check the configuration before proceeding.

```bash
# First, unbind from ftdi_sio if necessary
sudo modprobe -r ftdi_sio

# Read current EEPROM (backup)
sudo ftdi_eeprom --read-eeprom --device i:0x0403:0x6015 backup.eeprom

# Flash new configuration
sudo ftdi_eeprom --flash-eeprom infnoise-eeprom.conf

# Unplug and replug the device
```

### Verify

```bash
# Should now show 1209:3701
lsusb | grep 1209

# Device should show as "Infinite Noise TRNG"
lsusb -d 1209:3701 -v 2>/dev/null | grep -E "idVendor|idProduct|iProduct|iManufacturer"
```

## Building

### Prerequisites

- Linux 5.9 or newer, with the kernel headers for your running kernel
- GCC and make
- Root access for loading/testing

```bash
# Debian/Ubuntu
sudo apt install linux-headers-$(uname -r) build-essential

# Fedora/RHEL
sudo dnf install kernel-devel gcc make

# Arch Linux
sudo pacman -S linux-headers base-devel
```

### Compile

```bash
cd software/kernel
make
```

`make` does not compile in `dev_dbg()` output. To use the `debug` parameter without dynamic debug, build with `make INFNOISE_DEBUG=1`.

## Installation

### Manual Loading

```bash
# Load the module
sudo insmod infnoise.ko

# Verify it loaded
lsmod | grep infnoise
dmesg | grep infnoise
```

### Automatic Loading at Boot

```bash
# Copy module to kernel modules directory
sudo cp infnoise.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a

# Create modprobe configuration
echo "infnoise" | sudo tee /etc/modules-load.d/infnoise.conf

# Optional: set default parameters
echo "options infnoise multiplier=2" | sudo tee /etc/modprobe.d/infnoise.conf
```

### Unloading

```bash
sudo rmmod infnoise
```

## Configuration

### Module Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `debug` | bool | false | Enable verbose debug output |
| `raw_mode` | bool | false | Output raw (unwhitened) data on `/dev/infnoiseX` |
| `multiplier` | uint | 1 | `/dev/infnoiseX` Keccak output multiplier (0-10) |

These two settings do not affect `/dev/hwrng`. That interface always returns whitened data with no multiplier, because the kernel credits it at a fixed quality.

### Setting Parameters at Load Time

```bash
sudo insmod infnoise.ko debug=1 multiplier=4
```

### Changing Parameters at Runtime

```bash
# Enable debug output
echo 1 | sudo tee /sys/module/infnoise/parameters/debug

# Set multiplier to 4x
echo 4 | sudo tee /sys/module/infnoise/parameters/multiplier

# Check current values
cat /sys/module/infnoise/parameters/multiplier
```

### Multiplier Values

The `multiplier` parameter controls how much data `/dev/infnoiseX` squeezes from the Keccak sponge per entropy absorption:

| Value | Output per Round | Approximate Speed |
|-------|------------------|-------------------|
| 0 | Entropy bits only | ~50 Kbit/s |
| 1 | 32 bytes | ~124 Kbit/s |
| 2 | 64 bytes | ~249 Kbit/s |
| 4 | 128 bytes | ~499 Kbit/s |
| 10 | 320 bytes | ~1.2 Mbit/s |

Higher multiplier values produce more cryptographically secure pseudo-random data per true random sample. The output remains cryptographically secure due to the Keccak sponge construction.

## Usage

### Reading Random Data

```bash
# From hwrng interface
sudo dd if=/dev/hwrng bs=64 count=10 | xxd

# From character device
sudo dd if=/dev/infnoise0 bs=64 count=10 | xxd

# Generate a random file
sudo dd if=/dev/hwrng of=random.bin bs=1M count=10 iflag=fullblock
```

### Checking Device Status

```bash
# View kernel messages
dmesg | grep infnoise

# Check hwrng registration
cat /sys/class/misc/hw_random/rng_available

# Check current hwrng
cat /sys/class/misc/hw_random/rng_current
```

### FIPS 140-2 Testing

```bash
# Install rng-tools if needed
sudo apt install rng-tools  # Debian/Ubuntu

# Run FIPS tests
sudo dd if=/dev/hwrng bs=2500 count=100 iflag=fullblock | rngtest -c 100
```

### Automatic Entropy Feeding

The hwrng subsystem automatically feeds entropy to the kernel's random pool via an internal kernel thread (`hwrng_fillfn`). **No rngd daemon is required.**

The driver registers with quality=990. Each 256-bit output comes from one absorption of about 450 bits of entropy, so the output is close to full entropy.

A non-blocking read of `/dev/hwrng` returns `EAGAIN` during warmup or while another reader is using the device. Otherwise it makes one USB transfer, so it can still block briefly, and can return `EAGAIN` if the health check rejects that data.

The hwrng core thread continuously:
1. Reads data from the device
2. Credits entropy based on the quality setting
3. Mixes it into the kernel's CRNG via `add_hwgenerator_randomness()`

To verify automatic feeding is working:
```bash
# Check hwrng kernel thread exists
ps aux | grep hwrng

# Verify the device is active
cat /sys/class/misc/hw_random/rng_current
```

### Using with rngd (Optional, Legacy)

If you prefer manual control, you can use `rngd`:

```bash
# Install rng-tools
sudo apt install rng-tools

# Start rngd (it will auto-detect /dev/hwrng)
sudo rngd -r /dev/hwrng
```

This is redundant, since the hwrng core already handles entropy feeding.

## Troubleshooting

### Device Not Detected

**Check VID/PID:**
```bash
lsusb | grep -i "ftdi\|1209"
```

If you see `0403:6015`, the device needs to be reprogrammed. See [Device Setup](#device-setup).

**Check module is loaded:**
```bash
lsmod | grep infnoise
```

**Check for errors:**
```bash
dmesg | grep -i "infnoise\|1209:3701"
```

### Warmup Taking Too Long

The device requires approximately 450-500 warmup rounds before producing valid data. If warmup fails:

```bash
# Check dmesg for errors
dmesg | grep -i "infnoise.*failed\|infnoise.*error"

# Try reloading the module
sudo rmmod infnoise
sudo insmod infnoise.ko debug=1
```

### Health Check Failures

Occasional health check failures during warmup are normal, and the driver starts counting again, up to 10 times. A failure after warmup makes one read return `EIO`, and the driver then warms up again. It stops after more than 5 such failures, unless 10 minutes pass between two of them. If warmup fails or the driver stops, reads return `EIO` until the device is reconnected or the system resumes from sleep. This may indicate a hardware issue:

```bash
# Monitor health status
dmesg -w | grep infnoise
```

### Permission Denied

The device files are owned by root by default. To allow non-root access for one group:

```bash
# Create the group, and add your user to it
sudo groupadd --system infnoise
sudo usermod -aG infnoise "$USER"

# Create udev rule for permissions
cat << 'EOF' | sudo tee /etc/udev/rules.d/99-infnoise.rules
# Infinite Noise TRNG - allow group access
KERNEL=="infnoise*", GROUP="infnoise", MODE="0640"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
```

### Reverting EEPROM Changes

If you need to restore the original FTDI VID/PID:

```bash
# Create restore configuration
cat << 'EOF' > ftdi_restore.conf
vendor_id=0x0403
product_id=0x6015
manufacturer="FTDI"
product="FT240X"
EOF

# Flash (may need to specify current VID/PID)
sudo ftdi_eeprom --flash-eeprom --device i:0x1209:0x3701 ftdi_restore.conf
```

## Testing

```bash
# Read interfaces, ioctls, concurrent readers and entropy quality
sudo make test

# Also unbind and reset the device while it is open
sudo make test-destructive

# Also suspend the system for 10 seconds with the device open
sudo ./test_infnoise --destructive --suspend
```

Run the destructive tests on a kernel with `CONFIG_KASAN`, `CONFIG_PROVE_LOCKING` and `CONFIG_DETECT_HUNG_TASK`. They do not cover USB errors or a failed warmup, which need a failing device. To test these, unplug the device during a read.

## USB Device ID

| VID | PID | Description |
|-----|-----|-------------|
| 0x1209 | 0x3701 | Infinite Noise TRNG (pid.codes registered) |

The VID 0x1209 is provided by [pid.codes](https://pid.codes/), a registry for USB PID/VID for open source projects.

## Technical Details

### Health Monitoring

- Tracks 14 previous bits to predict next bit
- Uses fixed-point arithmetic (16.16 format)
- Requires ~80,000 bits warmup before data is valid
- Detects stuck-at faults (>20 consecutive identical bits)

## License

- Driver code: GPL-2.0-or-later
- Keccak implementation: CC0-1.0 (public domain)

## See Also

- [Infinite Noise TRNG Project](https://github.com/leetronics/infnoise)
- [Wayward Geek's Original Design](https://github.com/waywardgeek/infnoise)
