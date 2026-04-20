package org.cgutman.usbip.service;

import org.cgutman.usbip.server.protocol.UsbIpDevice;
import org.junit.Test;

import static org.junit.Assert.assertEquals;

public class UsbSpeedHeuristicsTest {

	@Test
	public void keepsConservativeChoiceForNonAudioIsoDevices() {
		int result = UsbSpeedHeuristics.detectSpeed(true, true, true, false, true, false, 0x200);

		assertEquals(UsbIpDevice.USB_SPEED_LOW, result);
	}

	@Test
	public void prefersHighSpeedForUsb2AudioIsoDevices() {
		int result = UsbSpeedHeuristics.detectSpeed(false, true, true, false, true, true, 0x200);

		assertEquals(UsbIpDevice.USB_SPEED_HIGH, result);
	}

	@Test
	public void prefersSuperSpeedForUsb3AudioIsoDevices() {
		int result = UsbSpeedHeuristics.detectSpeed(false, true, true, true, true, true, 0x300);

		assertEquals(UsbIpDevice.USB_SPEED_SUPER, result);
	}

	@Test
	public void fallsBackToFullSpeedWhenThatsTheOnlyCompatibleOption() {
		int result = UsbSpeedHeuristics.detectSpeed(false, true, false, false, true, true, 0x200);

		assertEquals(UsbIpDevice.USB_SPEED_FULL, result);
	}
}