// SPDX-License-Identifier: BSD-3-Clause
#include "usb/core.hxx"
#include "usb/device.hxx"

using namespace usb;
using namespace usb::types;
using namespace usb::core;
using usb::descriptors::usbDescriptor_t;
using usb::descriptors::usbEndpointDescriptor_t;
using usb::descriptors::usbEndpointType_t;
void usbHandleStatusCtrlEP() noexcept;

namespace usb::device
{
	setupPacket_t packet{};
	uint8_t activeConfig{};
	uint8_t activeAltMode{};

	namespace endpoint
	{
		uint8_t mapType(const usbEndpointType_t type)
		{
			switch (type)
			{
				case usbEndpointType_t::isochronous:
					return USB_EP_TYPE_ISOCHRONOUS_gc;
				case usbEndpointType_t::control:
					return USB_EP_TYPE_CONTROL_gc;
				default:
					break;
			}
			return USB_EP_TYPE_BULK_gc;
		}

		uint8_t mapMaxSize(const uint16_t size)
		{
			if (size <= 8)
				return USB_EP_BUFSIZE_8_gc;
			else if (size <= 16)
				return USB_EP_BUFSIZE_16_gc;
			else if (size <= 32)
				return USB_EP_BUFSIZE_32_gc;
			else if (size <= 64)
				return USB_EP_BUFSIZE_64_gc;
			// This should never happen..
			return USB_EP_BUFSIZE_1023_gc;
		}
	}

	void setupEndpoint(const usbEndpointDescriptor_t &endpoint)
	{
		if (endpoint.endpointType == usbEndpointType_t::control)
			return;

		const auto direction{static_cast<endpointDir_t>(endpoint.endpointAddress & ~usb::descriptors::endpointDirMask)};
		const auto endpointNumber{uint8_t(endpoint.endpointAddress & usb::descriptors::endpointDirMask)};
		auto &epCtrl
		{
			[direction](endpointCtrl_t &endpoint) -> USB_EP_t &
			{
				if (direction == endpointDir_t::controllerIn)
					return endpoint.controllerIn;
				else
					return endpoint.controllerOut;
			}(endpoints[endpointNumber])
		};

		epCtrl.CNT = 0;
		epCtrl.CTRL = endpoint::mapType(endpoint.endpointType) | endpoint::mapMaxSize(endpoint.maxPacketSize);
	}

	bool handleSetConfiguration() noexcept
	{
		usb::core::resetEPs(epReset_t::user);

		const auto config{packet.value.asConfiguration()};
		if (config > usb::configDescriptorCount)
			return false;
		activeConfig = config;

		if (activeConfig == 0)
			usbState = deviceState_t::addressed;
		else
		{
			const auto descriptors{*usb::descriptors::usbConfigDescriptors[activeConfig - 1]};
			for (const auto &part : descriptors)
			{
				flash_t<char *> descriptor{static_cast<const char *>(part.descriptor)};
				const auto type{static_cast<usbDescriptor_t>(descriptor[1])};
				if (type == usbDescriptor_t::endpoint)
				{
					const auto endpoint{*flash_t<usbEndpointDescriptor_t *>{part.descriptor}};
					setupEndpoint(endpoint);
				}
			}
		}

		return true;
	}

	answer_t handleStandardRequest() noexcept
	{
		const auto &epStatus{epStatusControllerIn[0]};

		switch (packet.request)
		{
			case request_t::setAddress:
				usbState = deviceState_t::addressing;
				return {response_t::zeroLength, nullptr, 0, memory_t::sram};
			case request_t::getDescriptor:
				return handleGetDescriptor();
			case request_t::setConfiguration:
				if (handleSetConfiguration())
					// Acknowledge the request.
					return {response_t::zeroLength, nullptr, 0, memory_t::sram};
				else
					// Bad request? Stall.
					return {response_t::stall, nullptr, 0, memory_t::sram};
			case request_t::getConfiguration:
				return {response_t::data, &activeConfig, 1, memory_t::sram};
			case request_t::getInterface:
				return {response_t::data, &activeAltMode, 1, memory_t::sram};
			case request_t::setInterface:
				if (packet.value == 0)
					// Valid alt-mode, ack it.
					return {response_t::zeroLength, nullptr, 0, memory_t::sram};
				else
					// Bad requeust? Stall.
					return {response_t::stall, nullptr, 0, memory_t::sram};
		}

		return {response_t::unhandled, nullptr, 0, memory_t::sram};
	}

	/*!
	 * @returns true when the all the data to be read has been retreived,
	 * false if there is more left to fetch.
	 */
	bool readCtrlEP() noexcept
	{
		auto &epStatus{epStatusControllerOut[0]};
		auto &ep0{endpoints[0].controllerOut};
		auto readCount{ep0.CNT};
		// Bounds sanity and then adjust how much is left to transfer
		if (readCount > epStatus.transferCount)
			readCount = epStatus.transferCount;
		epStatus.transferCount -= readCount;
		epStatus.memBuffer = recvData(0, epStatus.memBuffer, readCount);
		// Mark the recv buffer contents as done with
		ep0.CNT = 0;
		ep0.STATUS = 0;
		return !epStatus.transferCount;
	}

	/*!
	 * @returns true when the data to be transmitted is entirely sent,
	 * false if there is more left to send.
	 */
	bool writeCtrlEP() noexcept
	{
		auto &epStatus{epStatusControllerIn[0]};
		auto &ep0{endpoints[0].controllerIn};
		const auto sendCount{[&]() noexcept -> uint8_t
		{
			// Bounds sanity and then adjust how much is left to transfer
			if (epStatus.transferCount < epBufferSize)
				return epStatus.transferCount;
			return epBufferSize;
		}()};
		epStatus.transferCount -= sendCount;

		if (!epStatus.isMultiPart())
			epStatus.memBuffer = sendData(0, epStatus.memBuffer, sendCount);
		else
		{
			if (!epStatus.memBuffer)
				epStatus.memBuffer = (*epStatus.partsData.part(0)).descriptor;
			auto sendAmount{sendCount};
			uint8_t sendOffset{0};
			while (sendAmount)
			{
				const auto part{*epStatus.partsData.part(epStatus.partNumber)};
				auto *const begin{static_cast<const uint8_t *>(part.descriptor)};
				const auto partAmount{[&]() -> uint8_t
				{
					auto *const buffer{static_cast<const uint8_t *>(epStatus.memBuffer)};
					const auto amount{part.length - uint8_t(buffer - begin)};
					if (amount > sendAmount)
						return sendAmount;
					return amount;
				}()};
				sendAmount -= partAmount;
				epStatus.memBuffer = sendData(0, epStatus.memBuffer, partAmount, sendOffset);
				sendOffset += partAmount;
				// Get the buffer back to check if we exhausted it
				auto *const buffer{static_cast<const uint8_t *>(epStatus.memBuffer)};
				if (uint8_t(buffer - begin) == part.length &&
						epStatus.partNumber + 1 < epStatus.partsData.count())
					// We exhausted the chunk's buffer, so grab the next chunk
					epStatus.memBuffer = (*epStatus.partsData.part(++epStatus.partNumber)).descriptor;
			}
			if (!epStatus.transferCount)
				epStatus.isMultiPart(false);
		}
		// Mark the buffer as ready to send
		ep0.CNT = sendCount;
		ep0.STATUS &= ~(vals::usb::usbEPStatusNotReady | vals::usb::usbEPStatusNACK0);
		return !epStatus.transferCount;
	}

	void completeSetupPacket() noexcept
	{
		endpoints[0].controllerOut.STATUS &= ~vals::usb::usbEPStatusSetupComplete;
		// If we have no response
		if (!epStatusControllerIn[0].needsArming())
		{
			// But rather need more data
			if (epStatusControllerOut[0].needsArming())
			{
				// <SETUP[0]><OUT[1]><OUT[0]>...<IN[1]>
				usbCtrlState = ctrlState_t::dataRX;
			}
			// We need to stall in answer
			else if (epStatusControllerIn[0].stall())
			{
				auto &ep0{endpoints[0].controllerIn}; // EP0In
				// <SETUP[0]><STALL>
				ep0.CTRL |= vals::usb::usbEPCtrlStall;
				usbCtrlState = ctrlState_t::idle;
			}
		}
		// We have a valid response
		else
		{
			// Is this as part of a multi-part transaction?
			if (packet.requestType.dir() == endpointDir_t::controllerIn)
				// <SETUP[0]><IN[1]><IN[0]>...<OUT[1]>
				usbCtrlState = ctrlState_t::dataTX;
			// Or just a quick answer?
			else
				//  <SETUP[0]><IN[1]>
				usbCtrlState = ctrlState_t::statusTX;
			if (writeCtrlEP())
			{
				if (usbCtrlState == ctrlState_t::dataTX)
					usbCtrlState = ctrlState_t::statusRX;
				else
					usbCtrlState = ctrlState_t::idle;
			}
		}
	}

	void handleSetupPacket() noexcept
	{
		// Read in the new setup packet
		static_assert(sizeof(setupPacket_t) == 8); // Setup packets must be 8 bytes.
		epStatusControllerOut[0].memBuffer = &packet;
		epStatusControllerOut[0].transferCount = sizeof(setupPacket_t);
		if (!readCtrlEP())
		{
			// Truncated transfer.. WTF.
			auto &ep0{endpoints[0].controllerIn}; // Is this stall on EP0 In or Out?
			ep0.CTRL |= vals::usb::usbEPCtrlStall;
			return;
		}

		// Set up EP0 state for a reply of some kind
		//usbDeferalFlags = 0;
		usbCtrlState = ctrlState_t::wait;
		epStatusControllerIn[0].needsArming(false);
		epStatusControllerIn[0].stall(false);
		epStatusControllerIn[0].transferCount = 0;
		epStatusControllerOut[0].needsArming(false);
		epStatusControllerOut[0].stall(false);
		epStatusControllerOut[0].transferCount = 0;

		const auto &[response, data, size, memoryType] = handleStandardRequest();

		epStatusControllerIn[0].stall(response == response_t::stall || response == response_t::unhandled);
		epStatusControllerIn[0].needsArming(response == response_t::data || response == response_t::zeroLength);
		epStatusControllerIn[0].memBuffer = data;
		epStatusControllerIn[0].memoryType(memoryType);
		const uint16_t transferCount = response == response_t::zeroLength ? 0U : size;
		epStatusControllerIn[0].transferCount = std::min(transferCount, packet.length);
		// If the response is whacko, don't do the stupid thing
		if (response == response_t::data && !data && !epStatusControllerIn[0].isMultiPart())
			epStatusControllerIn[0].needsArming(false);
		completeSetupPacket();
		USB.INTFLAGSBCLR = vals::usb::itrStatusSetup;
	}

	void handleControllerOutPacket() noexcept
	{
		// If we're in the data phase
		if (usbCtrlState == ctrlState_t::dataRX)
		{
			if (readCtrlEP())
			{
				// If we now have all the data for the transaction..
				usbCtrlState = ctrlState_t::statusTX;
				// TODO: Handle data and generate status response.
			}
		}
		// If we're in the status phase
		else
			usbCtrlState = ctrlState_t::idle;
		endpoints[0].controllerOut.STATUS &= ~vals::usb::usbEPStatusIOComplete;
	}

	void handleControllerInPacket() noexcept
	{
		if (usbState == deviceState_t::addressing)
		{
			// We just handled an addressing request, and prepared our answer. Before we get a chance
			// to return from the interrupt that caused this chain of events, lets set the device address.
			const auto address{packet.value.asAddress()};

			// Check that the last setup packet was actually a set address request
			if (packet.requestType.type() != setupPacket::request_t::typeStandard ||
				packet.request != request_t::setAddress || address.addrH != 0)
			{
				USB.ADDR &= ~vals::usb::addressMask;
				usbState = deviceState_t::waiting;
			}
			else
			{
				USB.ADDR = address.addrL & vals::usb::addressMask;
				usbState = deviceState_t::addressed;
			}
		}

		// If we're in the data phase
		if (usbCtrlState == ctrlState_t::dataTX)
		{
			if (writeCtrlEP())
			{
				// If we now have all the data for the transaction..
				//usbCtrlState = ctrlState_t::statusRX;
				usbCtrlState = ctrlState_t::idle;
			}
		}
		// Otherwise this was a status phase TX-complete interrupt
		else
			usbCtrlState = ctrlState_t::idle;
		endpoints[0].controllerIn.STATUS &= ~vals::usb::usbEPStatusIOComplete;
	}

	void handleControlPacket() noexcept
	{
		// If we received a packet..
		if (usbPacket.dir() == endpointDir_t::controllerOut)
		{
			const auto status{endpoints[0].controllerOut.STATUS};
			if (status & vals::usb::usbEPStatusSetupComplete)
				handleSetupPacket();
			else
				handleControllerOutPacket();
		}
		else
			handleControllerInPacket();
	}
} // namespace usb::device
