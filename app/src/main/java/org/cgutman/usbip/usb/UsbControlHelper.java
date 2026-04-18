package org.cgutman.usbip.usb;

import android.hardware.usb.UsbConfiguration;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.os.Build;
import android.util.SparseArray;

import org.cgutman.usbip.service.AttachedDeviceContext;

public class UsbControlHelper {
	
	private static final int GET_DESCRIPTOR_REQUEST_TYPE = 0x80;
	private static final int GET_DESCRIPTOR_REQUEST = 0x06;
	
	private static final int GET_STATUS_REQUEST_TYPE = 0x82;
	private static final int GET_STATUS_REQUEST = 0x00;
	
	private static final int CLEAR_FEATURE_REQUEST_TYPE = 0x02;
	private static final int CLEAR_FEATURE_REQUEST = 0x01;

	private static final int SET_CONFIGURATION_REQUEST_TYPE = 0x00;
	private static final int SET_CONFIGURATION_REQUEST = 0x9;

	private static final int SET_INTERFACE_REQUEST_TYPE = 0x01;
	private static final int SET_INTERFACE_REQUEST = 0xB;

	private static final int FEATURE_VALUE_HALT = 0x00;
	
	private static final int DEVICE_DESCRIPTOR_TYPE = 1;

	/**
	 * Initializes the active interface map by selecting one alternate setting per interface
	 * (preferably alternate setting 0) from the current active configuration.
	 */
	public static void populateDefaultActiveInterfaces(AttachedDeviceContext deviceContext) {
		deviceContext.activeInterfacesById = new SparseArray<>();
		if (deviceContext.activeConfiguration == null) {
			return;
		}

		for (int i = 0; i < deviceContext.activeConfiguration.getInterfaceCount(); i++) {
			UsbInterface iface = deviceContext.activeConfiguration.getInterface(i);
			UsbInterface activeIface = deviceContext.activeInterfacesById.get(iface.getId());
			// Prefer alternate setting 0 when present, but keep a non-zero fallback when it is the only option.
			if (activeIface == null ||
					(activeIface.getAlternateSetting() != 0 && iface.getAlternateSetting() == 0)) {
				deviceContext.activeInterfacesById.put(iface.getId(), iface);
			}
		}
	}

	/**
	 * Rebuilds the endpoint cache from the currently active interface alternates.
	 */
	private static void populateActiveEndpointMap(AttachedDeviceContext deviceContext) {
		deviceContext.activeConfigurationEndpointsByNumDir = new SparseArray<>();
		if (deviceContext.activeInterfacesById == null) {
			return;
		}

		for (int i = 0; i < deviceContext.activeInterfacesById.size(); i++) {
			UsbInterface iface = deviceContext.activeInterfacesById.valueAt(i);
			for (int j = 0; j < iface.getEndpointCount(); j++) {
				UsbEndpoint endp = iface.getEndpoint(j);
				deviceContext.activeConfigurationEndpointsByNumDir.put(
						endp.getDirection() | endp.getEndpointNumber(),
						endp);
			}
		}
	}

	/**
	 * Releases all currently active interfaces tracked in the context.
	 */
	private static void releaseActiveInterfaces(AttachedDeviceContext deviceContext) {
		if (deviceContext.activeInterfacesById == null) {
			return;
		}

		for (int i = 0; i < deviceContext.activeInterfacesById.size(); i++) {
			UsbInterface iface = deviceContext.activeInterfacesById.valueAt(i);
			deviceContext.devConn.releaseInterface(iface);
		}
	}

	/**
	 * Claims all currently active interfaces tracked in the context.
	 */
	private static void claimActiveInterfaces(AttachedDeviceContext deviceContext) {
		if (deviceContext.activeInterfacesById == null) {
			return;
		}

		for (int i = 0; i < deviceContext.activeInterfacesById.size(); i++) {
			UsbInterface iface = deviceContext.activeInterfacesById.valueAt(i);
			if (!deviceContext.devConn.claimInterface(iface, true)) {
				System.err.println("Unable to claim interface: " + iface.getId());
			}
		}
	}

	public static UsbDeviceDescriptor readDeviceDescriptor(UsbDeviceConnection devConn) {
		byte[] descriptorBuffer = new byte[UsbDeviceDescriptor.DESCRIPTOR_SIZE];
		
		
		int res = XferUtils.doControlTransfer(devConn, GET_DESCRIPTOR_REQUEST_TYPE,
				GET_DESCRIPTOR_REQUEST,
				(DEVICE_DESCRIPTOR_TYPE << 8) | 0x00, // Devices only have 1 descriptor
				0, descriptorBuffer, descriptorBuffer.length, 0);
		if (res != UsbDeviceDescriptor.DESCRIPTOR_SIZE) {
			return null;
		}
		
		return new UsbDeviceDescriptor(descriptorBuffer);
	}
	
	public static boolean isEndpointHalted(UsbDeviceConnection devConn, UsbEndpoint endpoint) {
		byte[] statusBuffer = new byte[2];
		
		int res = XferUtils.doControlTransfer(devConn, GET_STATUS_REQUEST_TYPE,
				GET_STATUS_REQUEST,
				0,
				endpoint != null ? endpoint.getAddress() : 0,
				statusBuffer, statusBuffer.length, 0);
		if (res != statusBuffer.length) {
			return false;
		}
		
		return (statusBuffer[0] & 1) != 0;
	}
	
	public static boolean clearHaltCondition(UsbDeviceConnection devConn, UsbEndpoint endpoint) {
		int res = XferUtils.doControlTransfer(devConn, CLEAR_FEATURE_REQUEST_TYPE,
				CLEAR_FEATURE_REQUEST,
				FEATURE_VALUE_HALT,
				endpoint.getAddress(),
				null, 0, 0);
		if (res < 0) {
			return false;
		}
		
		return true;
	}

	public static boolean handleInternalControlTransfer(AttachedDeviceContext deviceContext, int requestType, int request, int value, int index) {
		// Mask out possible sign expansions
		requestType &= 0xFF;
		request &= 0xFF;
		value &= 0xFFFF;
		index &= 0xFFFF;

		if (requestType == SET_CONFIGURATION_REQUEST_TYPE && request == SET_CONFIGURATION_REQUEST) {
			System.out.println("Handling SET_CONFIGURATION via Android API");

			for (int i = 0; i < deviceContext.device.getConfigurationCount(); i++) {
				UsbConfiguration config = deviceContext.device.getConfiguration(i);
				if (config.getId() == value) {
					// If we have a current config, we need unclaim all interfaces to allow the
					// configuration change to work properly.
					if (deviceContext.activeConfiguration != null) {
						System.out.println("Unclaiming all interfaces from old configuration: "+deviceContext.activeConfiguration.getId());
						releaseActiveInterfaces(deviceContext);
					}

					if (!deviceContext.devConn.setConfiguration(config)) {
						// This can happen for certain types of devices where Android itself
						// has set the configuration for us. Let's just hope that whatever the
						// client wanted is also what Android selected :/
						System.err.println("Failed to set configuration! Proceeding anyway!");
					}

					// This is now the active configuration
					deviceContext.activeConfiguration = config;

					populateDefaultActiveInterfaces(deviceContext);
					populateActiveEndpointMap(deviceContext);

					System.out.println("Claiming all interfaces from new configuration: "+deviceContext.activeConfiguration.getId());
					claimActiveInterfaces(deviceContext);

					return true;
				}
			}

			System.err.printf("SET_CONFIGURATION specified invalid configuration: %d\n", value);
		}
		else if (requestType == SET_INTERFACE_REQUEST_TYPE && request == SET_INTERFACE_REQUEST) {
			System.out.println("Handling SET_INTERFACE via Android API");

			if (deviceContext.activeConfiguration != null) {
				for (int i = 0; i < deviceContext.activeConfiguration.getInterfaceCount(); i++) {
					UsbInterface iface = deviceContext.activeConfiguration.getInterface(i);
					if (iface.getId() == index && iface.getAlternateSetting() == value) {
						if (!deviceContext.devConn.setInterface(iface)) {
							System.err.println("Unable to set interface: "+iface.getId());
						}

						if (deviceContext.activeInterfacesById == null) {
							deviceContext.activeInterfacesById = new SparseArray<>();
						}
						
						// Do NOT release the previous interface before setting this one! 
						// Releasing the interface cancels pending URBs in usbfs and breaks SET_INTERFACE on Android.
						// We already claimed it, or we simply claim it along with others.
						UsbInterface previousIface = deviceContext.activeInterfacesById.get(index);
						if (previousIface != null && previousIface != iface) {
							// deviceContext.devConn.releaseInterface(previousIface);
						}

						deviceContext.activeInterfacesById.put(index, iface);
						// claimInterface is benign if already claimed, but helps if it was somehow skipped.
						if (!deviceContext.devConn.claimInterface(iface, true)) {
							System.err.println("Unable to claim interface: "+iface.getId());
						}
						populateActiveEndpointMap(deviceContext);
						return true;
					}
				}

				System.err.printf("SET_INTERFACE specified invalid interface: %d %d\n", index, value);
			}
			else {
				System.err.println("Attempted to use SET_INTERFACE before SET_CONFIGURATION!");
			}
		}

		return false;
	}
}
