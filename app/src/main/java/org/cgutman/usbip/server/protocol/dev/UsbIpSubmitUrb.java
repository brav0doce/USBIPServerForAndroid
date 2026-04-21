package org.cgutman.usbip.server.protocol.dev;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

import org.cgutman.usbip.utils.StreamUtils;

public class UsbIpSubmitUrb extends UsbIpDevicePacket {	
	private static final int MAX_ISO_PACKET_DESCRIPTORS = 16384;

	public int transferFlags;
	public int transferBufferLength;
	public int startFrame;
	public int numberOfPackets;
	public int interval;
	public byte[] setup;
	public UsbIpIsoPacketDescriptor[] isoPacketDescriptors;
	
	public static final int WIRE_SIZE =
			20 + 8;
	
	public byte[] outData;
	
	public UsbIpSubmitUrb(byte[] header) {
		super(header);
	}
	
	public static UsbIpSubmitUrb read(byte[] header, InputStream in) throws IOException {
		UsbIpSubmitUrb msg = new UsbIpSubmitUrb(header);
		
		byte[] continuationHeader = new byte[WIRE_SIZE];
		StreamUtils.readAll(in, continuationHeader);

		ByteBuffer bb = ByteBuffer.wrap(continuationHeader).order(ByteOrder.BIG_ENDIAN);
		msg.transferFlags = bb.getInt();
		msg.transferBufferLength = bb.getInt();
		msg.startFrame = bb.getInt();
		msg.numberOfPackets = bb.getInt();
		msg.interval = bb.getInt();
		
		msg.setup = new byte[8];
		bb.get(msg.setup);
		
		int paddingLength = UsbIpDevicePacket.USBIP_HEADER_SIZE - header.length - WIRE_SIZE;
		if (paddingLength > 0) {
			StreamUtils.readAll(in, new byte[paddingLength]);
		}

		if (msg.transferBufferLength < 0 || msg.numberOfPackets < 0) {
			throw new IOException("Invalid USB/IP submit lengths");
		}
		if (msg.numberOfPackets > MAX_ISO_PACKET_DESCRIPTORS) {
			throw new IOException("Too many ISO descriptors: " + msg.numberOfPackets);
		}
		
		long isoDescWireLengthLong = (long) msg.numberOfPackets * UsbIpIsoPacketDescriptor.WIRE_SIZE;
		if (isoDescWireLengthLong > Integer.MAX_VALUE) {
			throw new IOException("ISO descriptor payload too large");
		}
		int isoDescWireLength = (int) isoDescWireLengthLong;
		int outWireLength = msg.direction == UsbIpDevicePacket.USBIP_DIR_OUT ?
				msg.transferBufferLength : 0;
		long variableWireLengthLong = (long) outWireLength + isoDescWireLength;
		if (variableWireLengthLong > Integer.MAX_VALUE) {
			throw new IOException("USB/IP submit payload too large");
		}
		if (msg.direction == UsbIpDevicePacket.USBIP_DIR_OUT) {
			// Keep legacy behavior: always provide a non-null payload for OUT URBs, including zero-length ones.
			msg.outData = new byte[outWireLength];
		}
		int variableWireLength = (int) variableWireLengthLong;
		if (variableWireLength > 0) {
			byte[] variableData = new byte[variableWireLength];
			StreamUtils.readAll(in, variableData);

			if (msg.numberOfPackets > 0) {
				UsbIpIsoPacketDescriptor[] descFirst = UsbIpIsoPacketDescriptor.deserializeListWithFallback(
						variableData, 0, msg.numberOfPackets, msg.transferBufferLength);
				boolean firstPlausible = UsbIpIsoPacketDescriptor.looksPlausible(descFirst, msg.transferBufferLength);

				UsbIpIsoPacketDescriptor[] descLast = UsbIpIsoPacketDescriptor.deserializeListWithFallback(
						variableData, variableData.length - isoDescWireLength,
						msg.numberOfPackets, msg.transferBufferLength);
				boolean lastPlausible = UsbIpIsoPacketDescriptor.looksPlausible(descLast, msg.transferBufferLength);

				if (msg.direction == UsbIpDevicePacket.USBIP_DIR_OUT) {
					// Prefer the unique plausible layout. If both or neither look plausible, fall back
					// to descriptors-after-data because that is what Linux USB/IP implementations emit.
					boolean useDescFirst = firstPlausible && !lastPlausible;
					if (useDescFirst) {
						msg.isoPacketDescriptors = descFirst;
						System.arraycopy(variableData, isoDescWireLength, msg.outData, 0, outWireLength);
					}
					else {
						msg.isoPacketDescriptors = descLast;
						System.arraycopy(variableData, 0, msg.outData, 0, outWireLength);
					}
				}
				else {
					msg.isoPacketDescriptors = descFirst;
				}
			}
			else if (msg.direction == UsbIpDevicePacket.USBIP_DIR_OUT) {
				System.arraycopy(variableData, 0, msg.outData, 0, outWireLength);
			}
		}
		
		return msg;
	}
	
	@Override
	public String toString() {
		String sb = super.toString() +
				String.format("Xfer flags: 0x%x\n", transferFlags) +
				String.format("Xfer length: %d\n", transferBufferLength) +
				String.format("Start frame: %d\n", startFrame) +
				String.format("Number Of Packets: %d\n", numberOfPackets) +
				String.format("Interval: %d\n", interval) +
				String.format("Iso descriptors: %s\n",
						isoPacketDescriptors == null ? "none" : Arrays.toString(isoPacketDescriptors));
		return sb;
	}

	@Override
	protected byte[] serializeInternal() {
		throw new UnsupportedOperationException("Serializing not supported");
	}
}
