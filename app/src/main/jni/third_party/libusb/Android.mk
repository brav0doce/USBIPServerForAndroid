# Android.mk for vendored libusb
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := usb1_0
LOCAL_MODULE_FILENAME := libusb1.0

LOCAL_SRC_FILES := \
    libusb/core.c \
    libusb/descriptor.c \
    libusb/hotplug.c \
    libusb/io.c \
    libusb/sync.c \
    libusb/strerror.c \
    libusb/os/linux_usbfs.c \
    libusb/os/events_posix.c \
    libusb/os/threads_posix.c \
    libusb/os/linux_netlink.c

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/android \
    $(LOCAL_PATH)/libusb \
    $(LOCAL_PATH)/libusb/os

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/libusb
LOCAL_CFLAGS := -fvisibility=hidden -pthread
LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
