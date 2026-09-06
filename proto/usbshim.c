/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LD_PRELOAD shim that logs every libusb transfer the proprietary Pantum
 * backend makes. usbmon would need root; libusb interposition does not,
 * and it sees the same bytes.
 *
 * Build:  gcc -shared -fPIC -o usbshim.so usbshim.c -ldl
 * Use:    LD_PRELOAD=./usbshim.so PANTUM_SHIM_LOG=dump.txt scanimage ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

static FILE *logf;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Image data dwarfs the command traffic; past this a transfer is summarised. */
#define MAX_DUMP 512

static void shim_init(void)
{
    if (logf)
        return;
    const char *path = getenv("PANTUM_SHIM_LOG");
    logf = path ? fopen(path, "w") : stderr;
    if (!logf)
        logf = stderr;
    setvbuf(logf, NULL, _IOLBF, 0);
}

static void dump(const char *tag, int ep, const unsigned char *buf, int len, int req)
{
    pthread_mutex_lock(&lock);
    shim_init();
    fprintf(logf, "%s ep=0x%02x req=%d len=%d\n", tag, ep, req, len);
    int n = len > MAX_DUMP ? MAX_DUMP : len;
    for (int i = 0; i < n; i += 16) {
        fprintf(logf, "  %04x  ", i);
        for (int j = 0; j < 16; j++)
            if (i + j < n)
                fprintf(logf, "%02x ", buf[i + j]);
            else
                fprintf(logf, "   ");
        fprintf(logf, " |");
        for (int j = 0; j < 16 && i + j < n; j++) {
            unsigned char c = buf[i + j];
            fputc(c >= 32 && c < 127 ? c : '.', logf);
        }
        fprintf(logf, "|\n");
    }
    if (len > n)
        fprintf(logf, "  ... %d more bytes\n", len - n);
    pthread_mutex_unlock(&lock);
}

int libusb_bulk_transfer(void *dev_handle, unsigned char endpoint,
                         unsigned char *data, int length,
                         int *transferred, unsigned int timeout)
{
    static int (*real)(void *, unsigned char, unsigned char *, int, int *, unsigned int);
    if (!real)
        real = dlsym(RTLD_NEXT, "libusb_bulk_transfer");

    int is_in = endpoint & 0x80;
    if (!is_in)
        dump("OUT", endpoint, data, length, length);

    int rc = real(dev_handle, endpoint, data, length, transferred, timeout);

    if (is_in) {
        int got = (rc == 0 && transferred) ? *transferred : 0;
        dump("IN ", endpoint, data, got, length);
    }
    if (rc != 0) {
        pthread_mutex_lock(&lock);
        shim_init();
        fprintf(logf, "  !! rc=%d\n", rc);
        pthread_mutex_unlock(&lock);
    }
    return rc;
}

int libusb_control_transfer(void *dev_handle, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, unsigned char *data,
                            uint16_t wLength, unsigned int timeout)
{
    static int (*real)(void *, uint8_t, uint8_t, uint16_t, uint16_t,
                       unsigned char *, uint16_t, unsigned int);
    if (!real)
        real = dlsym(RTLD_NEXT, "libusb_control_transfer");

    int rc = real(dev_handle, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout);

    pthread_mutex_lock(&lock);
    shim_init();
    fprintf(logf, "CTRL type=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=%u rc=%d\n",
            bmRequestType, bRequest, wValue, wIndex, wLength, rc);
    pthread_mutex_unlock(&lock);
    if (rc > 0 && data)
        dump("CTRLDATA", 0, data, rc, wLength);
    return rc;
}

int libusb_claim_interface(void *dev_handle, int interface_number)
{
    static int (*real)(void *, int);
    if (!real)
        real = dlsym(RTLD_NEXT, "libusb_claim_interface");
    int rc = real(dev_handle, interface_number);
    pthread_mutex_lock(&lock);
    shim_init();
    fprintf(logf, "CLAIM iface=%d rc=%d\n", interface_number, rc);
    pthread_mutex_unlock(&lock);
    return rc;
}

int libusb_set_interface_alt_setting(void *dev_handle, int interface_number, int alternate_setting)
{
    static int (*real)(void *, int, int);
    if (!real)
        real = dlsym(RTLD_NEXT, "libusb_set_interface_alt_setting");
    int rc = real(dev_handle, interface_number, alternate_setting);
    pthread_mutex_lock(&lock);
    shim_init();
    fprintf(logf, "ALTSET iface=%d alt=%d rc=%d\n", interface_number, alternate_setting, rc);
    pthread_mutex_unlock(&lock);
    return rc;
}
