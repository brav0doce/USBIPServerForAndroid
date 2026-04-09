package org.cgutman.usbip.server.protocol.dev;

import org.junit.Test;

import java.io.ByteArrayInputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

public class UsbIpIsoProtocolTest {
	private static byte[] buildSubmitPacket(boolean descriptorFirst) {
		int transferLength = 8;
		int packetCount = 2;

		ByteBuffer header = ByteBuffer.allocate(20).order(ByteOrder.BIG_ENDIAN);
		header.putInt(UsbIpDevicePacket.USBIP_CMD_SUBMIT);
		header.putInt(101); // seq
		header.putInt(0x00010002); // dev id
		header.putInt(UsbIpDevicePacket.USBIP_DIR_OUT);
		header.putInt(1); // ep

		ByteBuffer submit = ByteBuffer.allocate(28).order(ByteOrder.BIG_ENDIAN);
		submit.putInt(0); // flags
		submit.putInt(transferLength);
		submit.putInt(0); // start frame
		submit.putInt(packetCount);
		submit.putInt(1); // interval
		submit.put(new byte[8]); // setup

		UsbIpIsoPacketDescriptor[] descriptors = new UsbIpIsoPacketDescriptor[packetCount];
		for (int i = 0; i < packetCount; i++) {
			descriptors[i] = new UsbIpIsoPacketDescriptor();
			descriptors[i].offset = i * 4;
			descriptors[i].length = 4;
			descriptors[i].actualLength = 0;
			descriptors[i].status = 0;
		}
		byte[] descriptorBytes = UsbIpIsoPacketDescriptor.serializeList(descriptors);
		byte[] outData = new byte[] { 10, 11, 12, 13, 20, 21, 22, 23 };

		ByteBuffer packet = ByteBuffer.allocate(header.capacity() + submit.capacity() +
				descriptorBytes.length + outData.length);
		packet.put(header.array());
		packet.put(submit.array());
		if (descriptorFirst) {
			packet.put(descriptorBytes);
			packet.put(outData);
		}
		else {
			packet.put(outData);
			packet.put(descriptorBytes);
		}

		return packet.array();
	}

	@Test
	public void parsesIsoSubmitWithDescriptorsFirst() throws Exception {
		byte[] raw = buildSubmitPacket(true);
		UsbIpSubmitUrb submit = (UsbIpSubmitUrb) UsbIpDevicePacket.read(new ByteArrayInputStream(raw));

		assertNotNull(submit);
		assertEquals(2, submit.numberOfPackets);
		assertNotNull(submit.isoPacketDescriptors);
		assertEquals(2, submit.isoPacketDescriptors.length);
		assertArrayEquals(new byte[] { 10, 11, 12, 13, 20, 21, 22, 23 }, submit.outData);
		assertEquals(0, submit.isoPacketDescriptors[0].offset);
		assertEquals(4, submit.isoPacketDescriptors[0].length);
		assertEquals(4, submit.isoPacketDescriptors[1].offset);
		assertEquals(4, submit.isoPacketDescriptors[1].length);
	}

	@Test
	public void parsesIsoSubmitWithDescriptorsLast() throws Exception {
		byte[] raw = buildSubmitPacket(false);
		UsbIpSubmitUrb submit = (UsbIpSubmitUrb) UsbIpDevicePacket.read(new ByteArrayInputStream(raw));

		assertNotNull(submit);
		assertEquals(2, submit.numberOfPackets);
		assertNotNull(submit.isoPacketDescriptors);
		assertEquals(2, submit.isoPacketDescriptors.length);
		assertArrayEquals(new byte[] { 10, 11, 12, 13, 20, 21, 22, 23 }, submit.outData);
		assertEquals(0, submit.isoPacketDescriptors[0].offset);
		assertEquals(4, submit.isoPacketDescriptors[0].length);
		assertEquals(4, submit.isoPacketDescriptors[1].offset);
		assertEquals(4, submit.isoPacketDescriptors[1].length);
	}

	@Test
	public void serializesIsoSubmitReplyWithPayloadAndDescriptors() {
		UsbIpSubmitUrbReply reply = new UsbIpSubmitUrbReply(1, 2, UsbIpDevicePacket.USBIP_DIR_IN, 3);
		reply.status = 0;
		reply.actualLength = 8;
		reply.numberOfPackets = 2;
		reply.inData = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 };

		UsbIpIsoPacketDescriptor d0 = new UsbIpIsoPacketDescriptor();
		d0.offset = 0;
		d0.length = 4;
		d0.actualLength = 4;
		d0.status = 0;

		UsbIpIsoPacketDescriptor d1 = new UsbIpIsoPacketDescriptor();
		d1.offset = 4;
		d1.length = 4;
		d1.actualLength = 4;
		d1.status = 0;

		reply.isoPacketDescriptors = new UsbIpIsoPacketDescriptor[] { d0, d1 };
		byte[] serialized = reply.serialize();

		int expectedLength = 20 + (UsbIpDevicePacket.USBIP_HEADER_SIZE - 20) + 8 + (2 * UsbIpIsoPacketDescriptor.WIRE_SIZE);
		assertEquals(expectedLength, serialized.length);

		ByteBuffer bb = ByteBuffer.wrap(serialized).order(ByteOrder.BIG_ENDIAN);
		bb.position(20 + (UsbIpDevicePacket.USBIP_HEADER_SIZE - 20));
		byte[] payload = new byte[8];
		bb.get(payload);
		assertArrayEquals(reply.inData, payload);

		UsbIpIsoPacketDescriptor[] descriptors = UsbIpIsoPacketDescriptor.deserializeList(
				serialized, 20 + (UsbIpDevicePacket.USBIP_HEADER_SIZE - 20) + 8, 2);
		assertEquals(0, descriptors[0].offset);
		assertEquals(4, descriptors[0].length);
		assertEquals(4, descriptors[1].offset);
		assertEquals(4, descriptors[1].actualLength);
	}
}
