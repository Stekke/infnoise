/* Library for the Infinite Noise Multiplier USB stick */

// Required to include clock_gettime
#define _POSIX_C_SOURCE 200809L

#define INFNOISE_VENDOR_ID 0x0403
#define INFNOISE_PRODUCT_ID 0x6015

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <ftdi.h>
#include "libinfnoise_private.h"
#include "KeccakF-1600-interface.h"

#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__FreeBSD__)
#include <fcntl.h>
#endif

uint8_t keccakState[KeccakPermutationSizeInBytes];
uint8_t outBuf[BUFLEN];

// The health check and these buffers are global, so a process can use one device
static bool infnoiseInUse = false;

// Not in the context, since its size is part of the library's binary interface
static bool usbOpen = false;
static char *deviceSerial = NULL;
// The last error was a failed health check, not a USB error
static bool healthFault = false;
// Samples thrown away in a row
static uint32_t rejectedSamples = 0u;

// 0403:6015 is shared by every FT-X chip, so the USB description tells us it is an INM.
// "FT240X" is what the EEPROM restore procedure in kernel/README.md writes.
static const char *const knownDescriptions[] = {"Infinite Noise TRNG", "FT240X USB FIFO", "FT240X"};
#define NUM_KNOWN_DESCRIPTIONS (sizeof(knownDescriptions) / sizeof(knownDescriptions[0]))

// Clear a buffer that held entropy, in a way the compiler can't optimize away.
void inmSecureZero(void *buf, size_t length) {
    volatile uint8_t *p = buf;
    while (length--) {
        *p++ = 0u;
    }
}

void prepareOutputBuffer() {
    uint32_t i;

    // Endless loop: set SW1EN and SW2EN alternately
    for (i = 0u; i < BUFLEN; i+=2) {
        // Alternate Ph1 and Ph2
        outBuf[i] = (1 << SWEN1);
        outBuf[i+1] = (1 << SWEN2);
    }
}

// The first attempt can fail while the device settles, so try twice.
static bool openDevice(struct infnoise_context *context) {
    if (!initializeUSB(&context->ftdic, &context->message, deviceSerial)
        && !initializeUSB(&context->ftdic, &context->message, deviceSerial)) {
        return false;
    }
    usbOpen = true;
    return true;
}

// Let the health check collect data.  Nothing read here is used as output.  A health
// fault can happen by chance, so start counting again, unless they keep happening.
static bool warmUp(struct infnoise_context *context) {
    uint32_t round;
    uint32_t healthFaults = 0u;

    rejectedSamples = 0u;
    for (round = 0u; round < MAX_WARMUP_ROUNDS; round++) {
        if (inmHealthCheckOkToUseData()) {
            rejectedSamples = 0u;
            return true;
        }
        readData(context, NULL, true, 1);
        if (!context->errorFlag) {
            continue;
        }
        if (!healthFault) {
            return false;
        }
        if (++healthFaults >= MAX_WARMUP_HEALTH_FAULTS) {
            return false;
        }
        context->errorFlag = false;
        healthFault = false;
        inmHealthCheckRestart();
    }
    context->message = "Unable to collect enough entropy to initialize health checker.";
    context->errorFlag = true;
    return false;
}

bool initInfnoise(struct infnoise_context *context, char *serial, bool keccak, bool debug) {
    context->message="";
    context->entropyThisTime=0;
    context->errorFlag=false;
    context->keccakBytesGiven=0;

    if (infnoiseInUse) {
        context->message = "Only one Infinite Noise Multiplier can be used per process";
        return false;
    }

    healthFault = false;
    usbOpen = false;
    deviceSerial = serial;

    prepareOutputBuffer();

    // initialize health check
    if (!inmHealthCheckStart(PREDICTION_BITS, DESIGN_K, debug)) {
        context->message = "Can't initialize health checker";
        return false;
    }

    // initialize USB
    if (!openDevice(context)) {
        inmHealthCheckStop();
        return false;
    }

    // initialize keccak
    if (keccak) {
        KeccakInitialize();
        KeccakInitializeState(keccakState);
    }

    // let healthcheck collect some data
    if (!warmUp(context)) {
        ftdi_deinit(&context->ftdic);
        usbOpen = false;
        inmHealthCheckStop();
        return false;
    }
    context->entropyThisTime = 0u;
    infnoiseInUse = true;
    return true;
}

bool reinitInfnoise(struct infnoise_context *context) {
    if (!infnoiseInUse) {
        context->message = "The Infinite Noise Multiplier is not initialized";
        return false;
    }
    if (usbOpen) {
        ftdi_deinit(&context->ftdic);
        usbOpen = false;
    }
    context->entropyThisTime = 0u;
    context->errorFlag = false;
    healthFault = false;
    context->keccakBytesGiven = 0u;
    inmHealthCheckRestart();

    if (!openDevice(context) || !warmUp(context)) {
        return false;
    }
    context->entropyThisTime = 0u;
    return true;
}

void deinitInfnoise(struct infnoise_context *context)
{
    inmHealthCheckStop();
    if (usbOpen) {
        ftdi_usb_close(&context->ftdic);
        ftdi_deinit(&context->ftdic);
        usbOpen = false;
    }
    inmSecureZero(keccakState, sizeof(keccakState));
    infnoiseInUse = false;
}


// Extract the INM output from the data received.  Basically, either COMP1 or COMP2
// changes, not both, so alternate reading bits from them.  We get 1 INM bit of output
// per byte read.  Feed bits from the INM to the health checker.  Return the expected
// bits of entropy.
uint32_t extractBytes(uint8_t *bytes, uint32_t length, uint8_t *inBuf, const char **message, bool *errorFlag) {
    inmClearEntropyLevel();
    uint32_t i;
    for (i = 0u; i < length; i++) {
        uint32_t j;
        uint8_t byte = 0u;
        for (j = 0u; j < 8u; j++) {
            uint8_t val = inBuf[i * 8u + j];
            uint8_t evenBit = (val >> COMP2) & 1u;
            uint8_t oddBit = (val >> COMP1) & 1u;
            bool even = j & 1u; // Use the even bit if j is odd
            uint8_t bit = even ? evenBit : oddBit;
            byte = (byte << 1u) | bit;

            // This is a good place to feed the bit from the INM to the health checker.
            if (!inmHealthCheckAddBit(evenBit, oddBit, even)) {
                *message = "Health check of Infinite Noise Multiplier failed!";
                *errorFlag = true;
                return 0;
            }
        }
        bytes[i] = byte;
    }
    return inmGetEntropyLevel();
}


// Return the difference in the times as a double in microseconds.
double diffTime(struct timespec *start, struct timespec *end) {
    uint32_t seconds = end->tv_sec - start->tv_sec;
    int32_t nanoseconds = end->tv_nsec - start->tv_nsec;
    return seconds * 1.0e6 + nanoseconds / 1000.0;
}

bool isSuperUser(void) {
    return (geteuid() == 0);
}

bool inmIsKnownDescription(const char *description) {
    uint32_t i;
    for (i = 0u; i < NUM_KNOWN_DESCRIPTIONS; i++) {
        if (strcmp(description, knownDescriptions[i]) == 0) {
            return true;
        }
    }
    return false;
}


// let's do it recursive, because if sth. fails we can easily wipe the malloc()
infnoise_devlist_node_t* inf_get_devstrings(struct ftdi_context* ftdic,
                                            struct ftdi_device_list* curdev,
                                            const char** message,
                                            infnoise_devlist_node_t* bgn,
                                            infnoise_devlist_node_t* end) {
    if( curdev != NULL ) {
        infnoise_devlist_node_t* cur;
        cur = (infnoise_devlist_node_t*) malloc (sizeof(infnoise_devlist_node_t));
        if (cur == NULL) {
            *message = "Out of memory";
            return NULL;
        }
        cur->next = NULL;
        int rc = ftdi_usb_get_strings(ftdic, curdev->dev,
                                      cur->manufacturer, sizeof(cur->manufacturer),
                                      cur->description, sizeof(cur->description),
                                      cur->serial, sizeof(cur->serial));
        if (rc < 0) {
            // Maybe some other FT-X product that we have no access to.  The message is
            // only used if no device can be listed.
            *message = ftdi_get_error_string(ftdic);
            free( cur );
            return inf_get_devstrings(ftdic, curdev->next, message, bgn, end);
        }
        else {
            // in case bgn is NULL, then implicitly end is NULL, also the other way around
            if(bgn == NULL) {
                bgn = cur;
            }
            else {
                end->next = cur;
            }
            infnoise_devlist_node_t* ret;
            ret = inf_get_devstrings(ftdic, curdev->next, message, bgn, cur);
            // a next dev triggered issue? -> wipe current
            if( ret == NULL ) {
                free( cur );
            }
            return ret;
        }
    }
    return bgn;
}


// Return a list of all FT-X devices found, since a device with a changed USB description
// can only be used with its serial.
infnoise_devlist_node_t* listUSBDevices(const char **message) {
    struct ftdi_context ftdic;
    *message = "Can't find Infinite Noise Multiplier.";
    if(ftdi_init(&ftdic) < 0) {
        *message = "Failed to init";
        return NULL;
    }

    infnoise_devlist_node_t* retlist = NULL;
    struct ftdi_device_list *devlist = NULL;
    if (ftdi_usb_find_all(&ftdic, &devlist, INFNOISE_VENDOR_ID, INFNOISE_PRODUCT_ID) < 0
        || devlist == NULL) {
        if (!isSuperUser()) {
            *message = "Can't find Infinite Noise Multiplier.  Try running as super user?";
        } else {
            *message = "Can't find Infinite Noise Multiplier.";
        }
    }
    else {
        retlist = inf_get_devstrings(&ftdic, devlist, message, NULL, NULL);
        ftdi_list_free2(devlist);
    }

    ftdi_deinit(&ftdic);
    return retlist;
}

// Read exactly length bytes.  libftdi returns early on a status-only USB packet, which
// the FT240X sends when it has no data at that moment, so a short read is not an error.
static bool readFully(struct ftdi_context *ftdic, uint8_t *buf, uint32_t length) {
    struct timespec start, now;
    uint32_t bytesRead = 0u;

    clock_gettime(CLOCK_MONOTONIC, &start);
    while (bytesRead < length) {
        int rc = ftdi_read_data(ftdic, buf + bytesRead, length - bytesRead);
        if (rc < 0) {
            return false;
        }
        bytesRead += rc;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (bytesRead < length && diffTime(&start, &now) > USB_READ_DEADLINE_MICROSEC) {
            return false;
        }
    }
    return true;
}

// Discard samples left by a failed transfer, since they would shift the even/odd phase.
static void drainSamples(struct ftdi_context *ftdic) {
    uint8_t buf[BUFLEN];
    uint32_t i;

    for (i = 0u; i < USB_MAX_DRAIN_READS; i++) {
        if (ftdi_read_data(ftdic, buf, sizeof(buf)) <= 0) {
            break;
        }
    }
    inmSecureZero(buf, sizeof(buf));
}

// Open the first device with this serial, or if serial is NULL, the first whose USB
// description is that of an INM.  Skip a device we can't read, which can be some other
// FT-X product that we have no access to, and report that in denied.  Set found if a
// device matches, and only then the result of opening it is returned.
static int openMatchingDevice(struct ftdi_context *ftdic, struct ftdi_device_list *devlist,
                              const char *serial, bool *found, bool *denied) {
    static bool multipleReported = false;
    struct ftdi_device_list *curdev;
    struct ftdi_device_list *match = NULL;
    bool multiple = false;

    for (curdev = devlist; curdev != NULL; curdev = curdev->next) {
        char description[128];
        char curSerial[128];
        if (ftdi_usb_get_strings(ftdic, curdev->dev, NULL, 0, description, sizeof(description),
                                 curSerial, sizeof(curSerial)) < 0) {
            *denied = true;
        } else if (serial != NULL) {
            if (strcmp(curSerial, serial) == 0) {
                match = curdev;
                break;
            }
        } else if (inmIsKnownDescription(description)) {
            multiple = match != NULL;
            if (match == NULL) {
                match = curdev;
            }
        }
    }
    if (match == NULL) {
        return 0;
    }
    *found = true;
    // Only once, since each recovery opens the device again
    if (multiple && !multipleReported) {
        fputs("Multiple Infinite Noise TRNGs found and serial not specified, using the first one\n", stderr);
        multipleReported = true;
    }
    return ftdi_usb_open_dev(ftdic, match->dev);
}

// Open and configure the device in an initialized ftdi context.
static bool setupUSB(struct ftdi_context *ftdic, const char **message, char *serial) {
    struct ftdi_device_list *devlist = NULL;

    // search devices
    int rc = ftdi_usb_find_all(ftdic, &devlist, INFNOISE_VENDOR_ID, INFNOISE_PRODUCT_ID);
    if (rc <= 0) {
        *message = "Can't find Infinite Noise Multiplier";
        return false;
    }

    bool found = false;
    bool denied = false;
    int openResult = openMatchingDevice(ftdic, devlist, serial, &found, &denied);
    ftdi_list_free2(devlist);

    if (found && openResult == -5) {
        *message = "Infinite Noise Multiplier is busy. Is another infnoise running?";
        return false;
    }
    if (!found && serial != NULL) {
        if (!isSuperUser()) {
            *message = "Can't find Infinite Noise Multiplier. Try running as super user?";
        } else {
            *message = "Can't find Infinite Noise Multiplier with given serial";
        }
        return false;
    }
    if (!found && !denied) {
        *message = "Can't find Infinite Noise Multiplier. Use --serial if its USB description was changed.";
        return false;
    }
    if (!found || openResult < 0) {
        if (!isSuperUser()) {
            *message = "Can't open Infinite Noise Multiplier. Try running as super user?";
        } else {
#if defined(__APPLE__)
            *message = "Can't open Infinite Noise Multiplier. sudo kextunload -b com.FTDI.driver.FTDIUSBSerialDriver ? sudo kextunload -b  com.apple.driver.AppleUSBFTDI ?";
#else
            *message = "Can't open Infinite Noise Multiplier.";
#endif
        }
        return false;
    }

    // Set high baud rate
    switch (ftdi_set_baudrate(ftdic, 30000)) {
    case -1:
        *message = "Invalid baud rate";
        return false;
    case -2:
        *message = "Setting baud rate failed";
        return false;
    case -3:
        *message = "Infinite Noise Multiplier unavailable";
        return false;
    default:
        break;
    }

    switch (ftdi_set_bitmode(ftdic, MASK, BITMODE_SYNCBB)) {
    case -1:
        *message = "Can't enable bit-bang mode";
        return false;
    case -2:
        *message = "Infinite Noise Multiplier unavailable\n";
        return false;
    default:
        break;
    }

    // A previous process can leave samples behind
    drainSamples(ftdic);

    // Just test to see that we can write and read.
    uint8_t buf[64u] = {0u,};
    if (ftdi_write_data(ftdic, buf, sizeof(buf)) != sizeof(buf)) {
        *message = "USB write failed";
        return false;
    }
    if (!readFully(ftdic, buf, sizeof(buf))) {
        *message = "USB read failed";
        return false;
    }
    return true;
}

// Initialize the Infinite Noise Multiplier USB interface.  On failure nothing is left
// open, since an open handle would keep the device busy for the next attempt.
bool initializeUSB(struct ftdi_context *ftdic, const char **message, char *serial) {
    if (ftdi_init(ftdic) < 0) {
        *message = "Failed to init";
        return false;
    }
    if (!setupUSB(ftdic, message, serial)) {
        ftdi_deinit(ftdic);
        return false;
    }
    return true;
}

// Output up to 1024 bits from the sponge.  result can be NULL during warmup.
static uint32_t squeeze(uint8_t *result, uint32_t *bytesGiven) {
    uint8_t dataOut[INFNOISE_MAX_RESULT];
    uint32_t bytesToWrite = INFNOISE_MAX_RESULT;

    if (bytesToWrite > *bytesGiven) {
        bytesToWrite = *bytesGiven;
    }

    KeccakExtract(keccakState, dataOut, bytesToWrite / 8u);
    KeccakPermutation(keccakState);
    if (result != NULL) {
        memcpy(result, dataOut, bytesToWrite);
    }
    inmSecureZero(dataOut, sizeof(dataOut));

    *bytesGiven -= bytesToWrite;
    return bytesToWrite;
}

// Clock out one batch of samples and read them back, retrying temporary USB errors.
// The time reported is that of the attempt which worked.
static bool transferSamples(struct infnoise_context *context, uint8_t *inBuf, double *microseconds) {
    uint32_t attempt;

    for (attempt = 0u; attempt < USB_MAX_ATTEMPTS; attempt++) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        // write clock signal
        if (ftdi_write_data(&context->ftdic, outBuf, sizeof(outBuf)) != sizeof(outBuf)) {
            context->message = "USB write failed";
        // and read 512 byte from the internal buffer (in synchronous bitbang mode)
        } else if (!readFully(&context->ftdic, inBuf, BUFLEN)) {
            context->message = "USB read failed";
        } else {
            clock_gettime(CLOCK_MONOTONIC, &end);
            *microseconds = diffTime(&start, &end);
            return true;
        }
        drainSamples(&context->ftdic);
    }
    context->errorFlag = true;
    return false;
}


// Whiten the output, if requested, with a Keccak sponge. Output bytes only if the health
// checker says it's OK.  Using outputMultiplier > 1 is a nice way to generate a lot more
// cryptographically secure pseudo-random data than the INM generates.  If
// outputMultiplier is 0, we output only as many bits as we measure in entropy.
// This allows a user to generate hundreds of MiB per second if needed, for use
// as cryptographic keys.
uint32_t processBytes(uint8_t *bytes, uint8_t *result, uint32_t *entropy,
                      uint32_t *bytesGiven,
                      bool raw, uint32_t outputMultiplier) {
    //Use the lower of the measured entropy and the provable lower bound on
    //average entropy.
    if (*entropy > inmExpectedEntropyPerBit * BUFLEN / INM_ACCURACY) {
        *entropy = inmExpectedEntropyPerBit * BUFLEN / INM_ACCURACY;
    }
    if (raw) {
        // In raw mode, we just output raw data from the INM.
        if (result != NULL) {
            memcpy(result, bytes, BUFLEN / 8u * sizeof(uint8_t));
        }
        return BUFLEN / 8u;
    }

    // Note that BUFLEN has to be less than 1600 by enough to make the sponge secure,
    // since outputting all 1600 bits would tell an attacker the Keccak state, allowing
    // him to predict any further output, when outputMultiplier > 1, until the next call
    // to processBytes.  All 512 bits are absorbed before squeezing data out to ensure that
    // we instantly recover (reseed) from a state compromise, which is when an attacker
    // gets a snapshot of the keccak state.  BUFLEN must be a multiple of 64, since
    // Keccak-1600 uses 64-bit "lanes".
    KeccakAbsorb(keccakState, bytes, BUFLEN / 64u);

    if (outputMultiplier == 0u) {
        // Output all the bytes of entropy we have
        uint8_t dataOut[BUFLEN / 8u];
        KeccakExtract(keccakState, dataOut, (*entropy + 63u) / 64u);
        if (result != NULL) {
            memcpy(result, dataOut, *entropy / 8u * sizeof(uint8_t));
        }
        inmSecureZero(dataOut, sizeof(dataOut));
        return *entropy / 8u;
    }

    // Output 256*outputMultipler bits (in chunks of 1024)
    // only the first 1024 now,
    *bytesGiven = outputMultiplier * 32u;
    return squeeze(result, bytesGiven);
}

uint32_t readData(struct infnoise_context *context, uint8_t *result, bool raw, uint32_t outputMultiplier) {
    // check if data can be squeezed from the keccak sponge from previous state (or we need to collect some new entropy to get bytesGiven >0)
    if (context->keccakBytesGiven != 0u) { // squeeze the sponge!
        return squeeze(result, &context->keccakBytesGiven);
    }

    // collect new entropy (e.g. case RAW)
    uint8_t inBuf[BUFLEN];
    uint8_t bytes[BUFLEN / 8u];
    double microseconds;
    uint32_t bytesWritten = 0u;
    const char *rejection = NULL;

    context->entropyThisTime = 0u;
    if (transferSamples(context, inBuf, &microseconds)) {
        if (microseconds > MAX_MICROSEC_FOR_SAMPLES) {
            rejection = "USB transfers from the Infinite Noise Multiplier are too slow";
        } else {
            uint32_t entropy = extractBytes(bytes, sizeof(bytes), inBuf, &context->message, &context->errorFlag);
            if (context->errorFlag) {
                healthFault = true;
            } else if (inmHealthCheckOkToUseData() && inmEntropyOnTarget(entropy, BUFLEN)) {
                // called health check are ok and return bytes
                context->entropyThisTime = entropy;
                bytesWritten = processBytes(bytes, result, &context->entropyThisTime, &context->keccakBytesGiven, raw, outputMultiplier);
                rejectedSamples = 0u;
            } else {
                rejection = "Entropy from the Infinite Noise Multiplier is out of range";
            }
        }
    }

    // A few rejected samples are normal, but don't keep trying without telling anyone
    if (rejection != NULL && ++rejectedSamples >= MAX_REJECTED_SAMPLES) {
        rejectedSamples = 0u;
        context->message = rejection;
        context->errorFlag = true;
    }
    inmSecureZero(inBuf, sizeof(inBuf));
    inmSecureZero(bytes, sizeof(bytes));
    return bytesWritten;
}
