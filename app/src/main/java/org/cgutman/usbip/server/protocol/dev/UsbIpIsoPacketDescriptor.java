package org.cgutman.usbip.server.protocol.dev;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class UsbIpIsoPacketDescriptor {
	public static final int WIRE_SIZE = 16;

	public int offset;
	public int length;
	public int actualLength;
	public int status;

	public static UsbIpIsoPacketDescriptor[] deserializeList(byte[] data, int offset, int count) {
		UsbIpIsoPacketDescriptor[] descriptors = new UsbIpIsoPacketDescriptor[count];
		ByteBuffer bb = ByteBuffer.wrap(data, offset, count * WIRE_SIZE).order(ByteOrder.BIG_ENDIAN);
		for (int i = 0; i < count; i++) {
			UsbIpIsoPacketDescriptor descriptor = new UsbIpIsoPacketDescriptor();
			descriptor.offset = bb.getInt();
			descriptor.length = bb.getInt();
			descriptor.actualLength = bb.getInt();
			descriptor.status = bb.getInt();
			descriptors[i] = descriptor;
		}
		return descriptors;
	}

	public static byte[] serializeList(UsbIpIsoPacketDescriptor[] descriptors) {
		if (descriptors == null || descriptors.length == 0) {
			return new byte[0];
		}

		ByteBuffer bb = ByteBuffer.allocate(descriptors.length * WIRE_SIZE).order(ByteOrder.BIG_ENDIAN);
		for (UsbIpIsoPacketDescriptor descriptor : descriptors) {
			bb.putInt(descriptor.offset);
			bb.putInt(descriptor.length);
			bb.putInt(descriptor.actualLength);
			bb.putInt(descriptor.status);
		}

		return bb.array();
	}

	public static boolean looksPlausible(UsbIpIsoPacketDescriptor[] descriptors, int transferBufferLength) {
		if (descriptors == null) {
			return false;
		}

		for (UsbIpIsoPacketDescriptor descriptor : descriptors) {
			if (descriptor == null) {
				return false;
			}

			if (descriptor.offset < 0 || descriptor.length < 0 ||
					descriptor.actualLength < 0 || descriptor.actualLength > descriptor.length) {
				return false;
			}

			if (descriptor.offset > transferBufferLength || descriptor.length > transferBufferLength) {
				return false;
			}

			if (descriptor.offset + descriptor.length > transferBufferLength) {
				return false;
			}
		}

		return true;
	}

	@Override
	public String toString() {
		return String.format("{off=%d len=%d actual=%d status=%d}",
				offset, length, actualLength, status);
	}
}
