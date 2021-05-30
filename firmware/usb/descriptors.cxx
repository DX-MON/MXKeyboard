// SPDX-License-Identifier: BSD-3-Clause
#include <array>
#include "usb/core.hxx"
#include "usb/descriptors.hxx"
#include "usb/device.hxx"
#include "usb/hid.hxx"

using namespace usb::descriptors;

static const usbDeviceDescriptor_t usbDeviceDesc
{
	sizeof(usbDeviceDescriptor_t),
	usbDescriptor_t::device,
	0x0200, // this is 2.00 in USB's BCD format
	usbClass_t::none,
	uint8_t(subclasses::device_t::none),
	uint8_t(protocols::device_t::none),
	usb::epBufferSize,
	usb::vid,
	usb::pid,
	0x0001, // BCD encoded device version
	1, // Manufacturer string index
	2, // Product string index
	0, // Temporarily do not support a serial number string
	usb::configDescriptorCount // One configuration only
};

static const usbDeviceQualifierDescriptor_t usbDeviceQualifierDesc
{
	sizeof(usbDeviceQualifierDescriptor_t),
	usbDescriptor_t::deviceQualifier,
	0x0200, // This is 2.00 in USB's BCD format
	usbClass_t::none,
	uint8_t(subclasses::device_t::none),
	uint8_t(protocols::device_t::none),
	usb::epBufferSize,
	0, // No other configurations
	0 // reserved field that must be 0.
};

static const std::array<usbConfigDescriptor_t, usb::configDescriptorCount> usbConfigDesc
{{
	{
		sizeof(usbConfigDescriptor_t),
		usbDescriptor_t::configuration,
		sizeof(usbConfigDescriptor_t) + sizeof(usbInterfaceDescriptor_t) +
			sizeof(hid::hidDescriptor_t) + sizeof(hid::reportDescriptor_t) +
			sizeof(usbEndpointDescriptor_t),
		usb::interfaceDescriptorCount,
		1, // This config
		4, // Configuration string index
		usbConfigAttr_t::defaults,
		250 // "500mA max", except we need 1A
	}
}};

static const std::array<usbInterfaceDescriptor_t, usb::interfaceDescriptorCount> usbInterfaceDesc
{{
	{
		sizeof(usbInterfaceDescriptor_t),
		usbDescriptor_t::interface,
		0, // interface index 0
		0, // alternate 0
		1, // one endpoint to the interface
		usbClass_t::hid,
		uint8_t(subclasses::hid_t::bootInterface),
		uint8_t(protocols::hid_t::keyboard),
		0 // No string to describe this interface (for now)
	}
}};

static const std::array<usbEndpointDescriptor_t, usb::endpointDescriptorCount> usbEndpointDesc
{{
	{
		sizeof(usbEndpointDescriptor_t),
		usbDescriptor_t::endpoint,
		endpointAddress(usbEndpointDir_t::controllerIn, 1),
		usbEndpointType_t::interrupt,
		usb::epBufferSize,
		1 // Poll once per frame
	}
}};

static const std::array<usbMultiPartDesc_t, 6> usbConfigSecs
{{
	{
		sizeof(usbConfigDescriptor_t),
		&usbConfigDesc[0]
	},
	{
		sizeof(usbInterfaceDescriptor_t),
		&usbInterfaceDesc[0]
	},
	{
		sizeof(hid::hidDescriptor_t),
		&usb::hid::usbKeyboardDesc
	},
	{
		sizeof(hid::reportDescriptor_t),
		&usb::hid::usbKeyboardReportDesc
	},
	{
		sizeof(usbEndpointDescriptor_t),
		&usbEndpointDesc[0]
	}
}};

namespace usb::descriptors
{
	const std::array<flash_t<usbMultiPartTable_t>, usb::configDescriptorCount> usbConfigDescriptors
	{{
		{{usbConfigSecs.begin(), usbConfigSecs.end()}}
	}};
} // namespace usb::descriptors

static const std::array<usbStringDesc_t, usb::stringCount + 1> usbStringDescs
{{
	{{u"\x0904", 1}},
	{{u"bad_alloc Heavy Industries", 26}},
	{{u"MXKeyboard", 10}},
	{{u"", 0}},
	{{u"HID keyboard interface", 22}}
}};

static const std::array<std::array<usbMultiPartDesc_t, 2>, usb::stringCount + 1> usbStringParts
{{
	usbStringDescs[0].asParts(),
	usbStringDescs[1].asParts(),
	usbStringDescs[2].asParts(),
	usbStringDescs[3].asParts(),
	usbStringDescs[4].asParts()
}};

static const std::array<flash_t<usbMultiPartTable_t>, usb::stringCount + 1> usbStrings
{{
	{{usbStringParts[0].begin(), usbStringParts[0].end()}},
	{{usbStringParts[1].begin(), usbStringParts[1].end()}},
	{{usbStringParts[2].begin(), usbStringParts[2].end()}},
	{{usbStringParts[3].begin(), usbStringParts[3].end()}},
	{{usbStringParts[4].begin(), usbStringParts[4].end()}},
}};

using namespace usb::types;
using namespace usb::core;

namespace usb::device
{
	answer_t handleGetDescriptor() noexcept
	{
		if (packet.requestType.dir() == endpointDir_t::controllerOut)
			return {response_t::unhandled, nullptr, 0, memory_t::sram};
		const auto descriptor = packet.value.asDescriptor();

		switch (descriptor.type)
		{
			// Handle device descriptor requests
			case usbDescriptor_t::device:
				return {response_t::data, &usbDeviceDesc, usbDeviceDesc.length, memory_t::flash};
			case usbDescriptor_t::deviceQualifier:
				static_assert(sizeof(usbDeviceQualifierDescriptor_t) == 10);
				return {response_t::data, &usbDeviceQualifierDesc, usbDeviceQualifierDesc.length, memory_t::flash};
			// Handle configuration descriptor requests
			case usbDescriptor_t::configuration:
			{
				if (descriptor.index >= configDescriptorCount)
					break;
				static_assert(sizeof(usbConfigDescriptor_t) == 9);
				static_assert(sizeof(usbInterfaceDescriptor_t) == 9);
				static_assert(sizeof(usbEndpointDescriptor_t) == 7);
				const auto configDescriptor{*usbConfigDescriptors[descriptor.index]};
				epStatusControllerIn[0].isMultiPart(true);
				epStatusControllerIn[0].partNumber = 0;
				epStatusControllerIn[0].partsData = configDescriptor;
				return {response_t::data, nullptr, configDescriptor.totalLength(), memory_t::flash};
			}
			// Handle interface descriptor requests
			case usbDescriptor_t::interface:
			{
				if (descriptor.index >= interfaceDescriptorCount)
					break;
				const auto &interfaceDescriptor{usbInterfaceDesc[descriptor.index]};
				return {response_t::data, &interfaceDescriptor, interfaceDescriptor.length, memory_t::flash};
			}
			// Handle endpoint descriptor requests
			case usbDescriptor_t::endpoint:
			{
				if (descriptor.index >= endpointDescriptorCount)
					break;
				const auto &endpointDescriptor{usbEndpointDesc[descriptor.index]};
				return {response_t::data, &endpointDescriptor, endpointDescriptor.length, memory_t::flash};
			}
			// Handle string requests
			case usbDescriptor_t::string:
			{
				if (descriptor.index >= stringCount)
					break;
				const auto &string{*usbStrings[descriptor.index]};
				epStatusControllerIn[0].isMultiPart(true);
				epStatusControllerIn[0].partNumber = 0;
				epStatusControllerIn[0].partsData = string;
				return {response_t::data, nullptr, string.totalLength(), memory_t::flash};
			}
			default:
				break;
		}
		return {response_t::unhandled, nullptr, 0, memory_t::sram};
	}
} // namespace usb::device
