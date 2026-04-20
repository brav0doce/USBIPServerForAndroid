package org.cgutman.usbip.service;

import org.cgutman.usbip.server.protocol.UsbIpDevice;

final class UsbSpeedHeuristics {

	private UsbSpeedHeuristics() {
	}

	static int detectSpeed(boolean lowPossible, boolean fullPossible, boolean highPossible,
						   boolean superPossible, boolean hasIsoEndpoint, boolean hasAudioInterface,
						   int bcdUsb) {
		if (hasIsoEndpoint && hasAudioInterface && bcdUsb >= 0x200) {
			if (superPossible && bcdUsb >= 0x300) {
				return UsbIpDevice.USB_SPEED_SUPER;
			}
			if (highPossible) {
				return UsbIpDevice.USB_SPEED_HIGH;
			}
			if (fullPossible) {
				return UsbIpDevice.USB_SPEED_FULL;
			}
			if (lowPossible) {
				return UsbIpDevice.USB_SPEED_LOW;
			}
			return UsbIpDevice.USB_SPEED_UNKNOWN;
		}

		if (lowPossible) {
			return UsbIpDevice.USB_SPEED_LOW;
		}
		else if (fullPossible) {
			return UsbIpDevice.USB_SPEED_FULL;
		}
		else if (highPossible) {
			return UsbIpDevice.USB_SPEED_HIGH;
		}
		else if (superPossible) {
			return UsbIpDevice.USB_SPEED_SUPER;
		}
		else {
			return UsbIpDevice.USB_SPEED_UNKNOWN;
		}
	}
}