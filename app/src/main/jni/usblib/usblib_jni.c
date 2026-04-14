#include <stdlib.h>
#include <unistd.h>
#include <jni.h>
#include <time.h>

#include <errno.h>
#include <limits.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>

JNIEXPORT jint JNICALL
Java_org_cgutman_usbip_jni_UsbLib_doBulkTransfer(
        JNIEnv *env, jclass clazz, jint fd, jint endpoint, jbyteArray data, jint timeout)
{
    jbyte* dataPtr = data ? (jbyte*)(*env)->GetPrimitiveArrayCritical(env, data, NULL) : NULL;
    jsize dataLen = data ? (*env)->GetArrayLength(env, data) : 0;

    struct usbdevfs_bulktransfer xfer = {
        .ep = endpoint,
        .len = dataLen,
        .timeout = timeout,
        .data = dataPtr,
    };
    jint res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_BULK, &xfer));
    if (res < 0) {
        res = -errno;
    }

    // If this is an OUT or a failed IN, use JNI_ABORT to avoid a useless memcpy().
    if (dataPtr) {
        (*env)->ReleasePrimitiveArrayCritical(env, data, dataPtr,
                                              ((endpoint & 0x80) && (res > 0)) ? 0 : JNI_ABORT);
    }

    return res;
};

JNIEXPORT jint JNICALL
Java_org_cgutman_usbip_jni_UsbLib_doControlTransfer(
        JNIEnv *env, jclass clazz, jint fd, jbyte requestType, jbyte request, jshort value,
        jshort index, jbyteArray data, jint length, jint timeout)
{
    (void)clazz;

    jsize dataLen = data ? (*env)->GetArrayLength(env, data) : 0;
    if (length < 0 || (length > 0 && data == NULL) || length > dataLen) {
        return -EINVAL;
    }

    jbyte* dataPtr = data ? (jbyte*)(*env)->GetPrimitiveArrayCritical(env, data, NULL) : NULL;

    struct usbdevfs_ctrltransfer xfer = {
            .bRequestType = requestType,
            .bRequest = request,
            .wValue = value,
            .wIndex = index,
            .wLength = length,
            .timeout = timeout,
            .data = dataPtr,
    };
    jint res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_CONTROL, &xfer));
    if (res < 0) {
        res = -errno;
    }

    // If this is an OUT or a failed IN, use JNI_ABORT to avoid a useless memcpy().
    if (dataPtr) {
        (*env)->ReleasePrimitiveArrayCritical(env, data, dataPtr,
                                              ((requestType & 0x80) && (res > 0)) ? 0 : JNI_ABORT);
    }

    return res;
};

static jintArray createIsoResultArray(JNIEnv *env, jint status, jint actual_length, jint error_count) {
    jintArray result = (*env)->NewIntArray(env, 3);
    if (result == NULL) {
        return NULL;
    }

    jint result_values[3];
    result_values[0] = status;
    result_values[1] = actual_length;
    result_values[2] = error_count;
    (*env)->SetIntArrayRegion(env, result, 0, 3, result_values);
    return result;
}

JNIEXPORT jintArray JNICALL
Java_org_cgutman_usbip_jni_UsbLib_doIsoTransfer(
        JNIEnv *env, jclass clazz, jint fd, jint endpoint, jbyteArray data, jint timeout,
        jintArray packet_lengths, jintArray packet_actual_lengths, jintArray packet_statuses)
{
    (void)clazz;

    if (packet_lengths == NULL || packet_actual_lengths == NULL || packet_statuses == NULL) {
        return createIsoResultArray(env, -EINVAL, 0, 0);
    }

    jsize packet_count = (*env)->GetArrayLength(env, packet_lengths);
    if (packet_count <= 0 ||
            packet_count != (*env)->GetArrayLength(env, packet_actual_lengths) ||
            packet_count != (*env)->GetArrayLength(env, packet_statuses)) {
        return createIsoResultArray(env, -EINVAL, 0, 0);
    }
    if (packet_count > 16384) {
        return createIsoResultArray(env, -EINVAL, 0, 0);
    }

    jsize data_len = data ? (*env)->GetArrayLength(env, data) : 0;
    jbyte *data_ptr = data ? (jbyte *)(*env)->GetPrimitiveArrayCritical(env, data, NULL) : NULL;
    jint *packet_lengths_ptr = (jint *)(*env)->GetPrimitiveArrayCritical(env, packet_lengths, NULL);
    jint *packet_actual_lengths_ptr = (jint *)(*env)->GetPrimitiveArrayCritical(env, packet_actual_lengths, NULL);
    jint *packet_statuses_ptr = (jint *)(*env)->GetPrimitiveArrayCritical(env, packet_statuses, NULL);

    if (packet_lengths_ptr == NULL || packet_actual_lengths_ptr == NULL || packet_statuses_ptr == NULL) {
        if (data_ptr) {
            (*env)->ReleasePrimitiveArrayCritical(env, data, data_ptr, JNI_ABORT);
        }
        if (packet_lengths_ptr) {
            (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, packet_lengths_ptr, JNI_ABORT);
        }
        if (packet_actual_lengths_ptr) {
            (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, packet_actual_lengths_ptr, JNI_ABORT);
        }
        if (packet_statuses_ptr) {
            (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, packet_statuses_ptr, JNI_ABORT);
        }
        return createIsoResultArray(env, -ENOMEM, 0, 0);
    }

    struct usbdevfs_urb *urb = calloc(1, sizeof(struct usbdevfs_urb) +
                                         (packet_count * sizeof(struct usbdevfs_iso_packet_desc)));
    if (urb == NULL) {
        if (data_ptr) {
            (*env)->ReleasePrimitiveArrayCritical(env, data, data_ptr, JNI_ABORT);
        }
        (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, packet_lengths_ptr, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, packet_actual_lengths_ptr, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, packet_statuses_ptr, JNI_ABORT);
        return createIsoResultArray(env, -ENOMEM, 0, 0);
    }

    long long total_requested_length = 0;
    for (jsize i = 0; i < packet_count; i++) {
        if (packet_lengths_ptr[i] < 0) {
            free(urb);
            if (data_ptr) {
                (*env)->ReleasePrimitiveArrayCritical(env, data, data_ptr, JNI_ABORT);
            }
            (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, packet_lengths_ptr, JNI_ABORT);
            (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, packet_actual_lengths_ptr, JNI_ABORT);
            (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, packet_statuses_ptr, JNI_ABORT);
            return createIsoResultArray(env, -EINVAL, 0, 0);
        }

        urb->iso_frame_desc[i].length = (unsigned int)packet_lengths_ptr[i];
        total_requested_length += packet_lengths_ptr[i];
        if (total_requested_length > INT_MAX) {
            free(urb);
            if (data_ptr) {
                (*env)->ReleasePrimitiveArrayCritical(env, data, data_ptr, JNI_ABORT);
            }
            (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, packet_lengths_ptr, JNI_ABORT);
            (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, packet_actual_lengths_ptr, JNI_ABORT);
            (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, packet_statuses_ptr, JNI_ABORT);
            return createIsoResultArray(env, -EINVAL, 0, 0);
        }
    }

    if (total_requested_length > (long long)data_len) {
        free(urb);
        if (data_ptr) {
            (*env)->ReleasePrimitiveArrayCritical(env, data, data_ptr, JNI_ABORT);
        }
        (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, packet_lengths_ptr, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, packet_actual_lengths_ptr, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, packet_statuses_ptr, JNI_ABORT);
        return createIsoResultArray(env, -EINVAL, 0, 0);
    }

    urb->type = USBDEVFS_URB_TYPE_ISO;
    urb->endpoint = (unsigned char) endpoint;
    urb->buffer = data_ptr;
    urb->buffer_length = data_len;
    urb->number_of_packets = packet_count;
    urb->usercontext = urb;

    jint res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_SUBMITURB, urb));
    if (res < 0) {
        res = -errno;
        free(urb);
        if (data_ptr) {
            (*env)->ReleasePrimitiveArrayCritical(env, data, data_ptr, JNI_ABORT);
        }
        (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, packet_lengths_ptr, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, packet_actual_lengths_ptr, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, packet_statuses_ptr, JNI_ABORT);
        return createIsoResultArray(env, res, 0, 0);
    }

    struct usbdevfs_urb *reaped_urb = NULL;
    if (timeout > 0) {
        struct timespec start_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);

        while (1) {
            res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_REAPURBNDELAY, &reaped_urb));
            if (res == 0 && reaped_urb != NULL) {
                break;
            }

            if (res < 0 && errno != EAGAIN) {
                res = -errno;
                break;
            }

            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            long elapsed_ms = ((now_ts.tv_sec - start_ts.tv_sec) * 1000L) +
                              ((now_ts.tv_nsec - start_ts.tv_nsec) / 1000000L);
            if (elapsed_ms >= timeout) {
                TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_DISCARDURB, urb));
                res = -ETIMEDOUT;
                break;
            }

            usleep(1000);
        }
    }
    else {
        res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_REAPURB, &reaped_urb));
        if (res < 0) {
            res = -errno;
        }
    }

    jint transfer_status = res;
    jint transfer_actual_length = 0;
    jint transfer_error_count = 0;
    if (res == 0 && reaped_urb != NULL) {
        transfer_status = reaped_urb->status < 0 ? reaped_urb->status : 0;
        transfer_actual_length = reaped_urb->actual_length;
        transfer_error_count = reaped_urb->error_count;
        for (jsize i = 0; i < packet_count; i++) {
            packet_actual_lengths_ptr[i] = (jint)reaped_urb->iso_frame_desc[i].actual_length;
            packet_statuses_ptr[i] = (jint)reaped_urb->iso_frame_desc[i].status;
        }
    }
    else if (transfer_status >= 0) {
        transfer_status = -EIO;
    }

    free(urb);

    if (data_ptr) {
        (*env)->ReleasePrimitiveArrayCritical(env, data, data_ptr,
                                              ((endpoint & 0x80) && (transfer_actual_length > 0)) ? 0 : JNI_ABORT);
    }
    (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, packet_lengths_ptr, JNI_ABORT);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, packet_actual_lengths_ptr, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, packet_statuses_ptr, 0);

    return createIsoResultArray(env, transfer_status, transfer_actual_length, transfer_error_count);
}

/*
 * ===========================================================================
 *  USB/IP Native Device Loop
 *  Implements the device-side (VHCI stub) of the USB/IP protocol per:
 *  https://docs.kernel.org/usb/usbip_protocol.html
 *
 *  Runs entirely in C, bypassing the JVM for maximum throughput.
 *  Fixes applied:
 *   - Reliable send/recv loops (handle partial TCP writes/reads)
 *   - Per-connection URB tracking (no global state leak between sessions)
 *   - Mutex-protected TCP writes (reaper + main thread share socket)
 *   - Proper error replies when ioctl SUBMITURB fails
 *   - Correct control-transfer endpoint handling (ep0 always 0 for usbfs)
 *   - Proper reaper thread lifecycle (join, not detach)
 *   - Correct ISO descriptor offset calculation
 * ===========================================================================
 */

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---- USB/IP wire protocol structures (48 bytes each) ---- */

struct usbip_header_basic {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
} __attribute__((packed));

struct usbip_header_cmd_submit {
    struct usbip_header_basic base;
    uint32_t transfer_flags;
    int32_t  transfer_buffer_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  interval;
    uint8_t  setup[8];
} __attribute__((packed));

struct usbip_header_ret_submit {
    struct usbip_header_basic base;
    int32_t status;
    int32_t actual_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t error_count;
    uint8_t padding[8];
} __attribute__((packed));

struct usbip_header_cmd_unlink {
    struct usbip_header_basic base;
    uint32_t unlink_seqnum;
    uint8_t  padding[24];
} __attribute__((packed));

struct usbip_header_ret_unlink {
    struct usbip_header_basic base;
    int32_t status;
    uint8_t padding[24];
} __attribute__((packed));

struct usbip_iso_packet_descriptor {
    uint32_t offset;
    uint32_t length;
    uint32_t actual_length;
    uint32_t status;
} __attribute__((packed));

/* ---- Per-URB context ---- */

struct urb_context {
    void*    buffer;
    uint32_t seqnum;     /* network byte order, as received */
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    int32_t  number_of_packets;
    struct urb_context* next;
    struct usbdevfs_urb urb;  /* variable-length (ISO descs at end) */
};

/* ---- Per-connection state ---- */

struct connection_state {
    int usbFd;
    int tcpFd;
    volatile int running;       /* set to 0 to signal shutdown */
    pthread_mutex_t send_mutex; /* protects all send() on tcpFd */
    pthread_mutex_t urb_mutex;  /* protects active_urbs list */
    struct urb_context* active_urbs;
};

/* ---- Reliable TCP I/O helpers ---- */

static int send_all(int fd, const void* buf, size_t len, pthread_mutex_t* mtx) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t remaining = len;
    if (mtx) pthread_mutex_lock(mtx);
    while (remaining > 0) {
        ssize_t n = send(fd, p, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            if (mtx) pthread_mutex_unlock(mtx);
            return -1;
        }
        p += n;
        remaining -= n;
    }
    if (mtx) pthread_mutex_unlock(mtx);
    return 0;
}

/* Send header + optional payload atomically (under one lock) */
static int send_header_and_data(int fd, const void* hdr, size_t hdr_len,
                                const void* data, size_t data_len,
                                const void* data2, size_t data2_len,
                                pthread_mutex_t* mtx) {
    const uint8_t* bufs[3]  = { hdr, data, data2 };
    size_t lens[3]          = { hdr_len, data_len, data2_len };

    if (mtx) pthread_mutex_lock(mtx);
    for (int i = 0; i < 3; i++) {
        if (!bufs[i] || lens[i] == 0) continue;
        const uint8_t* p = bufs[i];
        size_t rem = lens[i];
        while (rem > 0) {
            ssize_t n = send(fd, p, rem, MSG_NOSIGNAL);
            if (n <= 0) {
                if (mtx) pthread_mutex_unlock(mtx);
                return -1;
            }
            p += n;
            rem -= n;
        }
    }
    if (mtx) pthread_mutex_unlock(mtx);
    return 0;
}

static int recv_all(int fd, void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = recv(fd, p, remaining, MSG_WAITALL);
        if (n <= 0) return -1;
        p += n;
        remaining -= n;
    }
    return 0;
}

/* ---- URB list management (per-connection) ---- */

static void add_urb(struct connection_state* cs, struct urb_context* ctx) {
    pthread_mutex_lock(&cs->urb_mutex);
    ctx->next = cs->active_urbs;
    cs->active_urbs = ctx;
    pthread_mutex_unlock(&cs->urb_mutex);
}

static void remove_urb(struct connection_state* cs, struct urb_context* ctx) {
    pthread_mutex_lock(&cs->urb_mutex);
    struct urb_context** pp = &cs->active_urbs;
    while (*pp) {
        if (*pp == ctx) { *pp = ctx->next; break; }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&cs->urb_mutex);
}

static struct urb_context* find_urb_by_seqnum(struct connection_state* cs, uint32_t seqnum_net) {
    pthread_mutex_lock(&cs->urb_mutex);
    struct urb_context* c = cs->active_urbs;
    while (c) {
        if (c->seqnum == seqnum_net) {
            pthread_mutex_unlock(&cs->urb_mutex);
            return c;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&cs->urb_mutex);
    return NULL;
}

/* ---- Reaper thread: waits for completed URBs and sends replies ---- */

static void* usb_reaper_thread(void* arg) {
    struct connection_state* cs = (struct connection_state*)arg;

    while (cs->running) {
        struct usbdevfs_urb* reaped = NULL;
        int res = ioctl(cs->usbFd, USBDEVFS_REAPURB, &reaped);
        if (res < 0) {
            if (errno == ENODEV || !cs->running) break;
            if (errno == EINTR) continue;
            continue;
        }
        if (!reaped) continue;

        struct urb_context* ctx = (struct urb_context*)reaped->usercontext;
        if (!ctx) continue;

        remove_urb(cs, ctx);

        /* Build the RET_SUBMIT header */
        struct usbip_header_ret_submit ret;
        memset(&ret, 0, sizeof(ret));
        ret.base.command   = htonl(0x00000003); /* USBIP_RET_SUBMIT */
        ret.base.seqnum    = ctx->seqnum;
        ret.base.devid     = ctx->devid;
        ret.base.direction = ctx->direction;
        ret.base.ep        = ctx->ep;

        int actual = reaped->actual_length;
        ret.status          = htonl(reaped->status);
        ret.actual_length   = htonl(actual);
        ret.start_frame     = htonl(reaped->start_frame);
        ret.number_of_packets = htonl(ctx->number_of_packets);
        ret.error_count     = htonl(reaped->error_count);

        /* Determine payload to send back (IN direction only) */
        const void* payload_ptr = NULL;
        size_t payload_len = 0;
        int is_in = (ntohl(ctx->direction) != 0);

        if (is_in && ctx->buffer) {
            if (ctx->urb.type == USBDEVFS_URB_TYPE_CONTROL) {
                if (actual > 0) {
                    /* usbfs puts data after the 8-byte setup packet in the buffer */
                    payload_ptr = ((char*)ctx->buffer) + 8;
                    payload_len = actual;
                }
            } else if (ctx->urb.type == USBDEVFS_URB_TYPE_ISO) {
                /* USBIP expects ISO IN payloads to be tightly packed by actual_length 
                 * but usbfs spaces them by their maximum `length`. We must pack them 
                 * in-place before sending. */
                uint32_t current_offset = 0;
                uint32_t packed_offset = 0;
                for (int i = 0; i < ctx->number_of_packets; i++) {
                    uint32_t frame_len = reaped->iso_frame_desc[i].length;
                    uint32_t frame_act = reaped->iso_frame_desc[i].actual_length;
                    if (frame_act > 0 && packed_offset != current_offset) {
                        memmove(((uint8_t*)ctx->buffer) + packed_offset,
                                ((uint8_t*)ctx->buffer) + current_offset,
                                frame_act);
                    }
                    current_offset += frame_len;
                    packed_offset += frame_act;
                }
                if (packed_offset > 0) {
                    payload_ptr = ctx->buffer;
                    payload_len = packed_offset;
                }
                
                /* Ensure overall actual length matches packed length */
                ret.actual_length = htonl(packed_offset);
            } else {
                if (actual > 0) {
                    payload_ptr = ctx->buffer;
                    payload_len = actual;
                }
            }
        }

        /* Build ISO descriptors if needed */
        void* iso_buf = NULL;
        size_t iso_buf_len = 0;
        if (ctx->number_of_packets > 0) {
            iso_buf_len = ctx->number_of_packets * sizeof(struct usbip_iso_packet_descriptor);
            iso_buf = malloc(iso_buf_len);
            if (iso_buf) {
                struct usbip_iso_packet_descriptor* descs = (struct usbip_iso_packet_descriptor*)iso_buf;
                uint32_t offset = 0;
                for (int i = 0; i < ctx->number_of_packets; i++) {
                    descs[i].offset        = htonl(offset);
                    descs[i].length        = htonl(reaped->iso_frame_desc[i].length);
                    descs[i].actual_length = htonl(reaped->iso_frame_desc[i].actual_length);
                    descs[i].status        = htonl(reaped->iso_frame_desc[i].status);
                    offset += reaped->iso_frame_desc[i].length;
                }
            }
        }

        /* Send header + payload + ISO descs atomically */
        send_header_and_data(cs->tcpFd,
                             &ret, sizeof(ret),
                             payload_ptr, payload_len,
                             iso_buf, iso_buf_len,
                             &cs->send_mutex);

        if (iso_buf) free(iso_buf);
        if (ctx->buffer) free(ctx->buffer);
        free(ctx);
    }
    return NULL;
}

/* ---- Send an immediate error RET_SUBMIT for a failed ioctl ---- */

static void send_submit_error(struct connection_state* cs, struct urb_context* ctx, int err) {
    struct usbip_header_ret_submit ret;
    memset(&ret, 0, sizeof(ret));
    ret.base.command   = htonl(0x00000003);
    ret.base.seqnum    = ctx->seqnum;
    ret.base.devid     = ctx->devid;
    ret.base.direction = ctx->direction;
    ret.base.ep        = ctx->ep;
    ret.status         = htonl(err);

    /* For ISO failures, also send empty ISO descriptors */
    void* iso_buf = NULL;
    size_t iso_len = 0;
    if (ctx->number_of_packets > 0) {
        iso_len = ctx->number_of_packets * sizeof(struct usbip_iso_packet_descriptor);
        iso_buf = calloc(1, iso_len);
    }

    send_header_and_data(cs->tcpFd,
                         &ret, sizeof(ret),
                         NULL, 0,
                         iso_buf, iso_len,
                         &cs->send_mutex);
    if (iso_buf) free(iso_buf);
}

/* ---- Main entry point ---- */

JNIEXPORT jint JNICALL
Java_org_cgutman_usbip_jni_UsbLib_runNativeDeviceLoop(
        JNIEnv *env, jclass clazz, jint usbFd, jint tcpSocketFd)
{
    (void)env;
    (void)clazz;

    /* Ignore SIGPIPE so send() returns EPIPE instead of killing us */
    signal(SIGPIPE, SIG_IGN);

    /* Enable TCP_NODELAY for low latency */
    int flag = 1;
    setsockopt(tcpSocketFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* Per-connection state (heap-allocated so reaper thread has stable pointer) */
    struct connection_state* cs = calloc(1, sizeof(struct connection_state));
    if (!cs) return -1;
    cs->usbFd   = usbFd;
    cs->tcpFd    = tcpSocketFd;
    cs->running  = 1;
    pthread_mutex_init(&cs->send_mutex, NULL);
    pthread_mutex_init(&cs->urb_mutex, NULL);
    cs->active_urbs = NULL;

    pthread_t reaper;
    if (pthread_create(&reaper, NULL, usb_reaper_thread, cs) != 0) {
        free(cs);
        return -1;
    }

    /* Main command loop: reads commands from the TCP socket */
    while (cs->running) {
        struct usbip_header_basic base;
        if (recv_all(tcpSocketFd, &base, sizeof(base)) < 0) break;

        uint32_t cmd = ntohl(base.command);

        if (cmd == 0x00000001) { /* USBIP_CMD_SUBMIT */
            struct usbip_header_cmd_submit sub;
            sub.base = base;
            if (recv_all(tcpSocketFd, ((uint8_t*)&sub) + 20, 28) < 0) break;

            int is_out     = (ntohl(sub.base.direction) == 0);
            int32_t buf_len = (int32_t)ntohl(sub.transfer_buffer_length);
            int num_iso    = (int32_t)ntohl(sub.number_of_packets);
            uint32_t ep_num = ntohl(sub.base.ep);

            /* Sanity: limit buffer size */
            if (buf_len < 0) buf_len = 0;
            if (buf_len > 16 * 1024 * 1024) { break; }
            if (num_iso < 0) num_iso = 0;

            int data_wire  = is_out ? buf_len : 0;
            int iso_wire   = num_iso * (int)sizeof(struct usbip_iso_packet_descriptor);

            /* Read OUT payload from network */
            void* buffer = NULL;
            if (data_wire > 0) {
                buffer = malloc(data_wire);
                if (!buffer) break;
                if (recv_all(tcpSocketFd, buffer, data_wire) < 0) { free(buffer); break; }
            }

            /* Read ISO descriptors from network */
            void* iso_in = NULL;
            if (iso_wire > 0) {
                iso_in = malloc(iso_wire);
                if (!iso_in) { if (buffer) free(buffer); break; }
                if (recv_all(tcpSocketFd, iso_in, iso_wire) < 0) {
                    free(iso_in);
                    if (buffer) free(buffer);
                    break;
                }
            }

            /* Allocate urb_context (with room for ISO frame descriptors) */
            struct urb_context* ctx = calloc(1,
                sizeof(struct urb_context) + sizeof(struct usbdevfs_iso_packet_desc) * num_iso);
            if (!ctx) {
                if (buffer) free(buffer);
                if (iso_in) free(iso_in);
                break;
            }

            ctx->seqnum    = sub.base.seqnum;
            ctx->devid     = sub.base.devid;
            ctx->direction = sub.base.direction;
            ctx->ep        = sub.base.ep;
            ctx->number_of_packets = num_iso;

            struct usbdevfs_urb* urb = &ctx->urb;

            /* Determine URB type */
            if (num_iso > 0) {
                urb->type = USBDEVFS_URB_TYPE_ISO;
                struct usbip_iso_packet_descriptor* d =
                    (struct usbip_iso_packet_descriptor*)iso_in;
                for (int i = 0; i < num_iso; i++) {
                    urb->iso_frame_desc[i].length = ntohl(d[i].length);
                    urb->iso_frame_desc[i].actual_length = 0;
                    urb->iso_frame_desc[i].status = 0;
                }
            } else if (ep_num == 0) {
                urb->type = USBDEVFS_URB_TYPE_CONTROL;
            } else {
                urb->type = USBDEVFS_URB_TYPE_BULK;
            }
            if (iso_in) free(iso_in);

            /* Set endpoint byte: For control transfers (ep0), usbfs wants endpoint=0
               (direction is embedded in the setup packet). For others, set 0x80 for IN. */
            if (urb->type == USBDEVFS_URB_TYPE_CONTROL) {
                urb->endpoint = 0;
            } else {
                urb->endpoint = ep_num & 0xFF;
                if (!is_out) urb->endpoint |= 0x80;
            }

            /* Build the data buffer */
            if (urb->type == USBDEVFS_URB_TYPE_CONTROL) {
                /* usbfs control: 8-byte setup packet prepended to data buffer */
                void* ctrl = malloc(8 + buf_len);
                if (!ctrl) {
                    if (buffer) free(buffer);
                    free(ctx);
                    break;
                }
                memcpy(ctrl, sub.setup, 8);
                if (buffer && data_wire > 0) {
                    memcpy(ctrl + 8, buffer, data_wire);
                    free(buffer);
                }
                ctx->buffer = ctrl;
                urb->buffer = ctrl;
                urb->buffer_length = 8 + buf_len;
            } else {
                /* For IN: allocate receive buffer if needed */
                if (!buffer && buf_len > 0) {
                    buffer = calloc(1, buf_len);
                    if (!buffer) { free(ctx); break; }
                }
                ctx->buffer = buffer;
                urb->buffer = buffer;
                urb->buffer_length = buf_len;
            }

            urb->start_frame     = (num_iso > 0) ? 0 : 0; /* let kernel pick */
            urb->number_of_packets = num_iso;
            urb->usercontext     = ctx;

            /* For ISO, set ASAP flag so kernel schedules immediately */
            if (urb->type == USBDEVFS_URB_TYPE_ISO) {
                urb->flags = USBDEVFS_URB_ISO_ASAP;
            }

            add_urb(cs, ctx);

            int res = ioctl(usbFd, USBDEVFS_SUBMITURB, urb);
            if (res < 0) {
                /* Submit failed: remove from list and send error reply immediately */
                remove_urb(cs, ctx);
                send_submit_error(cs, ctx, -errno);
                if (ctx->buffer) free(ctx->buffer);
                free(ctx);
            }

        } else if (cmd == 0x00000002) { /* USBIP_CMD_UNLINK */
            struct usbip_header_cmd_unlink unl;
            unl.base = base;
            if (recv_all(tcpSocketFd, ((uint8_t*)&unl) + 20, 28) < 0) break;

            /* unlink_seqnum is in network byte order on the wire */
            struct urb_context* target = find_urb_by_seqnum(cs, unl.unlink_seqnum);
            if (target) {
                ioctl(usbFd, USBDEVFS_DISCARDURB, &target->urb);
                /* The reaper thread will see the discarded URB and send RET_SUBMIT
                   with status -ECONNRESET. We still need to send RET_UNLINK. */
            }

            struct usbip_header_ret_unlink ret;
            memset(&ret, 0, sizeof(ret));
            ret.base.command   = htonl(0x00000004);
            ret.base.seqnum    = unl.base.seqnum;
            ret.base.devid     = unl.base.devid;
            ret.base.direction = unl.base.direction;
            ret.base.ep        = unl.base.ep;
            ret.status = htonl(target ? -ECONNRESET : -ENOENT);

            send_all(cs->tcpFd, &ret, sizeof(ret), &cs->send_mutex);

        } else {
            break; /* Unknown command */
        }
    }

    /* Shutdown: signal reaper, then unblock its blocking REAPURB ioctl.
     * We do NOT close usbFd here - Java manages the USB connection lifecycle.
     * Instead, submit a DISCARDURB for any outstanding URBs and use
     * ioctl(USBDEVFS_DISCARDURB) to wake the reaper. */
    cs->running = 0;
    shutdown(tcpSocketFd, SHUT_RDWR);

    /* Discard all outstanding URBs so the reaper's REAPURB unblocks */
    pthread_mutex_lock(&cs->urb_mutex);
    struct urb_context* u = cs->active_urbs;
    while (u) {
        ioctl(usbFd, USBDEVFS_DISCARDURB, &u->urb);
        u = u->next;
    }
    pthread_mutex_unlock(&cs->urb_mutex);

    pthread_join(reaper, NULL);

    /* Free any remaining URBs */
    struct urb_context* c = cs->active_urbs;
    while (c) {
        struct urb_context* next = c->next;
        if (c->buffer) free(c->buffer);
        free(c);
        c = next;
    }

    pthread_mutex_destroy(&cs->send_mutex);
    pthread_mutex_destroy(&cs->urb_mutex);
    free(cs);

    return 0;
}
