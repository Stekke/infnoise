/* Driver for the Infinite Noise Multiplier USB stick */

// Required to include clock_gettime
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__FreeBSD__)
#include <fcntl.h>
#endif

#include <string.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include "infnoise.h"
#include "libinfnoise_private.h"

// The largest value that fits the 32-bit arithmetic on it
#define MAX_MULTIPLIER (UINT32_MAX / 32u)
#define MAX_DIVIDER 1024u
#define MAX_FEED_FREQ ((uint32_t)(INT_MAX / 1000))

// Tries to get the device working again.  They are forgotten once it has worked for a
// while, so that a device which keeps failing does end the process.
#define MAX_RECOVERIES 5u
#define RECOVERY_DELAY_SECONDS 1u
#define RECOVERY_FORGET_SECONDS 600

volatile sig_atomic_t running = 1;
static volatile sig_atomic_t recovering = 0;

void term(int signum)
{
   (void) signum;
   running = 0;
   // The warmup can take minutes on a bad device, and there is nothing to save
   if (recovering) {
       _exit(0);
   }
}

static time_t monotonicSeconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

static void initOpts(struct opt_struct *opts) {
    opts->outputMultiplier = 0u;
    opts->inputDivider = 1u;
    opts->feedFreq = 30u;
    opts->forceReseed = false;
    opts->daemon = false;
    opts->debug = false;
    opts->devRandom = false;
    opts->noOutput = false;
    opts->listDevices = false;
    opts->raw = false;
    opts->version = false;
    opts->help = false;
    opts->none = false;
    opts->pidFileName =
    opts->serial = NULL;
}

// getopt_long(3) options descriptor
static struct option longopts[] = {
        {"raw",          no_argument,       NULL, 'r'},
        {"debug",        no_argument,       NULL, 'D'},
        {"dev-random",   no_argument,       NULL, 'R'},
        {"reseed-crng",  no_argument,       NULL, 'C'},
        {"no-output",    no_argument,       NULL, 'n'},
        {"feed-frequency",    required_argument, NULL, 'f'},
        {"multiplier",   required_argument, NULL, 'm'},
        {"divider",      required_argument, NULL, 'x'},
        {"pidfile",      required_argument, NULL, 'p'},
        {"serial",       required_argument, NULL, 's'},
        {"daemon",       no_argument,       NULL, 'd'},
        {"list-devices", no_argument,       NULL, 'l'},
        {"version",      no_argument,       NULL, 'v'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL,           0,                 NULL, 0}};

// Parse a whole decimal number between min and max.
static bool parseNumber(const char *text, uint32_t min, uint32_t max, uint32_t *value) {
    char *end;
    unsigned long number;

    errno = 0;
    number = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || text[0] == '-'
        || number < min || number > max) {
        return false;
    }
    *value = (uint32_t)number;
    return true;
}

// Write the bytes to either stdout, or /dev/random.
bool outputBytes(uint8_t *bytes, uint32_t length, uint32_t entropy, bool writeDevRandom, bool forceKernelReseed, uint32_t feedFrequency, const char **message) {
    if (!writeDevRandom) {
        if (fwrite(bytes, 1, length, stdout) != length) {
            *message = "Unable to write output from Infinite Noise Multiplier";
            return false;
        }
    } else {
#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__FreeBSD__)
        // quell compiler warning about unused variable.
        static int devRandomFD = -1;
        (void) entropy;

        if (devRandomFD < 0)
            devRandomFD = open("/dev/random", O_WRONLY);
        if (devRandomFD < 0) {
            *message = "Unable to open random(4)";
            return false;
        };
        // we are not trapping EINT and EAGAIN; as the random(4) driver seems
        // to not treat partial writes as not an error. So we think that comparing
        // to length is fine.
        //
        if (write(devRandomFD, bytes, length) != length) {
            *message = "Unable to write output from Infinite Noise Multiplier to random(4)";
            return false;
        }
#endif
#if defined(__APPLE__)
        *message = "macOS doesn't support writes to entropy pool";
        // suppress warning
        (void) entropy;
        (void) feedFrequency;
        (void) forceKernelReseed;
        return false;
#endif
#ifdef LINUX
        inmWaitForPoolToHaveRoom(feedFrequency);
        if (!inmWriteEntropyToPool(bytes, length, entropy)) {
            *message = "Unable to add entropy to /dev/random";
            return false;
        }
        if(forceKernelReseed) {
            inmForceKernelRngReseed();
        }
#endif
    }
    return true;
}

int main(int argc, char **argv) {
    struct infnoise_context context;
    struct opt_struct opts;
    int ch;
    bool multiplierAssigned = false;
    bool dividerAssigned = false;

    initOpts(&opts);

    // Process arguments
    while ((ch = getopt_long(argc, argv, "rDRCnf:m:x:p:s:dlvh", longopts, NULL)) !=
           -1) {
        switch (ch) {
            case 'r':
                opts.raw = true;
                break;
            case 'D':
                opts.debug = true;
                break;
            case 'R':
                opts.devRandom = true;
                break;
            case 'C':
                opts.forceReseed = true;
                break;
            case 'n':
                opts.noOutput = true;
                break;
            case 'f':
                if (!parseNumber(optarg, 0u, MAX_FEED_FREQ, &opts.feedFreq)) {
                    fprintf(stderr, "feed-freq must be between 0 and %u\n", MAX_FEED_FREQ);
                    return 1;
                }
                break;
            case 'm':
                multiplierAssigned = true;
                if (!parseNumber(optarg, 0u, MAX_MULTIPLIER, &opts.outputMultiplier)) {
                    fprintf(stderr, "Multiplier must be between 0 and %u\n", MAX_MULTIPLIER);
                    return 1;
                }
                break;
            case 'x':
                dividerAssigned = true;
                if (!parseNumber(optarg, 1u, MAX_DIVIDER, &opts.inputDivider)) {
                    fprintf(stderr, "Divider must be between 1 and %u\n", MAX_DIVIDER);
                    return 1;
                }
                break;
            case 'p':
                opts.pidFileName = optarg;
                if (opts.pidFileName == NULL || opts.pidFileName[0] == '\0') {
                    fputs("--pidfile without file name\n", stderr);
                    return 1;
                }
                break;
            case 's':
                opts.serial = optarg;
                if (opts.serial == NULL || opts.serial[0] == '\0') {
                    fputs("--serial without value\n", stderr);
                    return 1;
                }
                break;
            case 'd':
                opts.daemon = true;
                break;
            case 'l':
                opts.listDevices = true;
                break;
            case 'v':
                opts.version = true;
                break;
            case 'h':
                opts.help = true;
                break;
            default:
                opts.help = true;
                opts.none = true;
        }
    }

    if (opts.help) {
        fputs("Usage: infnoise [options]\n"
              "Options are:\n"
              "    -D, --debug - turn on some debug output\n"
              "    -R, --dev-random - write entropy to /dev/random instead of "
              "stdout\n"
              "    -C, --reseed-crng - force reseed of /dev/random on write\n"
              "    -r, --raw - do not whiten the output\n"
              "    -m, --multiplier <value> - write 256 bits * value for each 512 bits written to\n"
              "      the Keccak sponge.  Default of 0 means write all the entropy.\n"
              "    -x, --divider <value> - write 512 bits for each <value> times 512 bits written to\n"
              "      the Keccak sponge.  Default of 1 means no compression.\n"
              "    -f, --feed-frequency - feed interval for /dev/random, in seconds, 0 means no wait (default: 30)\n"
              "    -n, --no-output - do not write random output data\n"
              "    -p, --pidfile <file> - write process ID to file\n"
              "    -d, --daemon - run in the background\n"
              "    -s, --serial <serial> - use specified device\n"
              "    -l, --list-devices - list available devices\n"
              "    -v, --version - show version information\n"
              "    -h, --help - this help output\n",
              stdout);
        return opts.none;
    }

    if (opts.version) {
        printf("GIT VERSION - %s\n", GIT_VERSION);
        printf("GIT COMMIT  - %s\n", GIT_COMMIT);
        printf("GIT DATE    - %s\n", GIT_DATE);
        return 0;
    }

    // read environment variables, not overriding command line options
    if (opts.serial == NULL) {
        opts.serial = getenv("INFNOISE_SERIAL");
        if (opts.serial != NULL && opts.serial[0] == '\0') {
            opts.serial = NULL;
        }
    }

    if (!opts.debug) {
        char *envDbg = getenv("INFNOISE_DEBUG");
        opts.debug = (envDbg != NULL
                      && !strcmp("true", envDbg));
    }

    if (!multiplierAssigned) {
        char *envMultiplier = getenv("INFNOISE_MULTIPLIER");
        if (envMultiplier != NULL && envMultiplier[0] != '\0') {
            if (!parseNumber(envMultiplier, 0u, MAX_MULTIPLIER, &opts.outputMultiplier)) {
                fprintf(stderr, "Multiplier must be between 0 and %u\n", MAX_MULTIPLIER);
                return 1;
            }
            multiplierAssigned = true;
        }
    }

    if (!dividerAssigned) {
        char *envDivider = getenv("INFNOISE_DIVIDER");
        if (envDivider != NULL && envDivider[0] != '\0'
            && !parseNumber(envDivider, 1u, MAX_DIVIDER, &opts.inputDivider)) {
            fprintf(stderr, "Divider must be between 1 and %u\n", MAX_DIVIDER);
            return 1;
        }
    }

    if (opts.inputDivider > 1u && multiplierAssigned) {
        fputs("Divider above 1 can't be used with a multiplier\n", stderr);
        return 1;
    }
    if (opts.inputDivider > 1u && opts.raw) {
        fputs("Divider above 1 can't be used with raw output\n", stderr);
        return 1;
    }

    if (!multiplierAssigned && opts.devRandom) {
        opts.outputMultiplier = 2u; // Don't throw away entropy when writing to /dev/random unless told to do so
    }

    if (opts.listDevices) {
        infnoise_devlist_node_t* devlist = listUSBDevices(&context.message);
        if (devlist == NULL) {
            fprintf(stderr, "Error: %s\n", context.message);
            return 1;
        }
        infnoise_devlist_node_t* curdev;
        int id = 0;
        for (curdev = devlist; curdev != NULL; id++) {
            printf("ID: %i, Manufacturer: %s, Description: %s, Serial: %s%s\n",
                   id, curdev->manufacturer, curdev->description, curdev->serial,
                   inmIsKnownDescription(curdev->description) ? "" : " (unknown description, needs --serial)");

            // cleanup:
            infnoise_devlist_node_t* olddev = curdev;
            curdev = curdev->next;
            free(olddev);
        }
        return 0;
    }

    if (opts.devRandom) {
#ifdef LINUX
        inmWriteEntropyStart(INFNOISE_MAX_RESULT, opts.debug); // TODO: create method in libinfnoise.h for this?
        if (!opts.noOutput && !inmCanWriteEntropy()) {
            perror("Unable to add entropy to /dev/random, which needs root or CAP_SYS_ADMIN");
            return 1;
        }
#endif
#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__FreeBSD__)
        int devRandomFD = open("/dev/random", O_WRONLY);
        if (devRandomFD < 0) {
            fprintf(stderr, "Unable to open /dev/random\n");
            exit(1);
        }
        close(devRandomFD);
#endif
#if defined(__APPLE__)
        context.message = "dev/random not supported on macOS";
        fprintf(stderr, "Error: %s\n", context.message);
        return 1;
#endif
    }

    // Optionally run in the background and optionally write a PID-file
    startDaemon(&opts);

    // initialize USB device, health check and Keccak state (see libinfnoise)
    if (!initInfnoise(&context, opts.serial, !opts.raw, opts.debug)) {
        fprintf(stderr, "Error: %s\n", context.message);
        return 1; // ERROR
    }

    daemonReady();

    // calculate output size based on the parameters:
    uint64_t resultSize;
    if (opts.outputMultiplier <= 2 || opts.raw) {
        resultSize = 64u;
    } else {
        resultSize = 128u;
    }
    //fprintf(stderr, "resultsize: %lu\n", resultSize);

    // get proper shutdown
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = term;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    // endless loop
    uint64_t totalBytesWritten = 0u;
    uint32_t recoveries = 0u;
    time_t lastFailure = 0;
    while (running) {
        uint8_t result[resultSize];
        uint64_t bytesWritten = opts.inputDivider > 1u
                                ? readDataDivided(&context, result, opts.inputDivider)
                                : readData(&context, result, opts.raw, opts.outputMultiplier);

        if (context.errorFlag) {
            bool recovered = false;
            fprintf(stderr, "Error: %s\n", context.message);
            if (monotonicSeconds() - lastFailure >= RECOVERY_FORGET_SECONDS) {
                recoveries = 0u;
            }
            recovering = 1;
            while (!recovered && running && recoveries < MAX_RECOVERIES) {
                recoveries++;
                sleep(RECOVERY_DELAY_SECONDS);
                recovered = reinitInfnoise(&context);
                if (!recovered) {
                    fprintf(stderr, "Error: %s\n", context.message);
                }
            }
            recovering = 0;
            if (!recovered && running) {
                fputs("Error: Unable to recover the Infinite Noise Multiplier\n", stderr);
                return 1;
            }
            lastFailure = monotonicSeconds();
            continue;
        }

        // Nothing passed the checks, so there is nothing to output or credit
        if (bytesWritten == 0u) {
            continue;
        }
        totalBytesWritten += bytesWritten;

        // Credit a sample's entropy once, and no more than the bytes can hold
        uint32_t entropy = context.entropyThisTime;
        if (entropy > bytesWritten * 8u) {
            entropy = bytesWritten * 8u;
        }
        context.entropyThisTime = 0u;

        if (!opts.noOutput
            && !outputBytes(result, bytesWritten, entropy, opts.devRandom, opts.forceReseed, opts.feedFreq, &context.message)) {
            fprintf(stderr, "Error: %s\n", context.message);
            return 1;
        }
        inmSecureZero(result, sizeof(result));

        if (opts.debug &&
            totalBytesWritten / (1u << 20u) != (totalBytesWritten - bytesWritten) / (1u << 20u)) {
            fprintf(stderr, "Output %lu bytes\n", (unsigned long) totalBytesWritten);
        }
    }
    deinitInfnoise(&context);
#ifdef LINUX
    inmWriteEntropyEnd();
#endif
    return 0;
}
