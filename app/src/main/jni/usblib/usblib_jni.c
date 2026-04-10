#include <stdlib.h>
#include <unistd.h>
#include <jni.h>
#include <time.h>

#include <errno.h>
#include <limits.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

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
