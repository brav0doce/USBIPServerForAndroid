# Android.mk for usblib
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := usblib
LOCAL_SRC_FILES := usblib_jni.c
LOCAL_SHARED_LIBRARIES := usb1_0
LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../third_party/libusb \
	$(LOCAL_PATH)/../third_party/libusb/libusb \
	$(LOCAL_PATH)/../third_party/libusb/android
LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
