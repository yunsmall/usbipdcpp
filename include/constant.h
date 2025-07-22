#pragma once

#include <cstdint>

namespace usbipdcpp {

    constexpr std::uint16_t EP0_MAX_PACKET_SIZE = 64;

    enum class UsbSpeed {
        Unknown = 0x0,
        Low,
        Full,
        High,
        Wireless,
        Super,
        SuperPlus,
    };

    enum class ClassCode {
        SeeInterface = 0,
        Audio,
        CDC,
        HID,
        Physical = 0x05,
        Image,
        Printer,
        MassStorage,
        Hub,
        CDCData,
        SmartCard,
        ContentSecurity = 0x0D,
        Video,
        PersonalHealthcare,
        AudioVideo,
        Billboard,
        TypeCBridge,
        Diagnostic = 0xDC,
        WirelessController = 0xE0,
        Misc = 0xEF,
        ApplicationSpecific = 0xFE,
        VendorSpecific = 0xFF,
    };

    enum class Direction {
        In,
        Out,
    };

    enum class EndpointAttributes {
        Control = 0,
        Isochronous,
        Bulk,
        Interrupt,
    };

    enum class StandardRequest {
        GetStatus = 0,
        ClearFeature = 1,
        SetFeature = 3,
        SetAddress = 5,
        GetDescriptor = 6,
        SetDescriptor = 7,
        GetConfiguration = 8,
        SetConfiguration = 9,
        GetInterface = 10,
        SetInterface = 11,
        SynchFrame = 12,
    };

    enum class HIDRequest {
        GetReport = 0x01,
        GetIdle = 0x02,
        GetProtocol = 0x03,
        SetReport = 0x09,
        SetIdle = 0x0A,
        SetProtocol = 0x0B,
    };

    enum class HIDReportType {
        Output = 0x01,
        Input = 0x02,
        Feature = 0x03,
    };
    enum class HIDProtocolType {
        Boot = 0x00,
        Report = 0x01,
    };

    enum class RequestRecipient {
        Device = 0,
        Interface,
        Endpoint,
        Other
    };

    enum class DescriptorType {
        Device = 1,
        Configuration = 2,
        String = 3,
        Interface = 4,
        Endpoint = 5,
        DeviceQualifier = 6,
        OtherSpeedConfiguration = 7,
        InterfacePower = 8,
        OTG = 9,
        Debug = 0xA,
        InterfaceAssociation = 0xB,
        BOS = 0xF,
    };

    enum HidDescriptorType {
        Hid = 0x21,
        Report = 0x22,
        Physical = 0x23,
    };

    enum class PortFeat {
        Connection = 0,
        Enable = 1,
        Suspend = 2, /* L2 suspend */
        OverCurrent = 3,
        Reset = 4,
        L1 = 5, /* L1 suspend */
        Power = 8,
        LowSpeed = 9, /* Should never be used */
        C_Connection = 16,
        C_Enable = 17,
        C_Suspend = 18,
        C_OverCurrent = 19,
        C_Reset = 20,
        Test = 21,
        Indicator = 22,
        C_PortL1 = 23,
    };

    enum class RequestType {
        Standard = (0x00 << 5),
        Class = (0x01 << 5),
        Vendor = (0x02 << 5),
        Reserved = (0x03 << 5)
    };

    enum class TransferFlag {
        URB_SHORT_NOT_OK = 0x0001, /* report short reads as errors */
        URB_ISO_ASAP = 0x0002, /* iso-only; use the first unexpired */
        /* slot in the schedule */
        URB_NO_TRANSFER_DMA_MAP = 0x0004, /* urb->transfer_dma valid on submit */
        URB_NO_FSBR = 0x0020, /* UHCI-specific */
        URB_ZERO_PACKET = 0x0040, /* Finish bulk OUT with short packet */
        URB_NO_INTERRUPT = 0x0080, /* HINT: no non-error interrupt */
        /* needed */
        URB_FREE_BUFFER = 0x0100,
    };
}
