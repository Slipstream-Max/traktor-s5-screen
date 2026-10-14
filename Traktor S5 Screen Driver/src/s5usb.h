#pragma once

#include <ntddk.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdf.h>
#include <wdfusb.h>

// The vendor function code observed in nikst4usb.sys.
#define FILE_DEVICE_S5 0x8089
#define IOCTL_S5_WRITE_BULK \
    CTL_CODE(FILE_DEVICE_S5, 0xA20, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S5_QUERY_STATUS \
    CTL_CODE(FILE_DEVICE_S5, 0xA21, METHOD_BUFFERED, FILE_ANY_ACCESS)

// This is the interface class GUID observed in a live NIHardwareService
// SetupAPI trace while it searched for the S5 display function. The driver
// publishes only this verified production interface.
DEFINE_GUID(GUID_DEVINTERFACE_NI_HWS_DISPLAY,
    0xc757f7b0, 0xa56e, 0x4d3d, 0xb8, 0xd4, 0x95, 0x16, 0xe9, 0x32, 0xca, 0xf7);

#define S5_USB_INTERFACE_CLASS 0xff
#define S5_USB_INTERFACE_SUBCLASS 0xbd
#define S5_USB_BULK_OUT 0x03
#define S5_DISPLAY_MODE 3
#define S5_MAX_FRAME_SIZE (4u * 1024u * 1024u)

typedef struct _S5_DEVICE_CONTEXT {
    WDFUSBDEVICE UsbDevice;
    WDFUSBINTERFACE UsbInterface;
    WDFUSBPIPE BulkOut;
    WDFQUEUE IoQueue;
    ULONG InterfaceNumber;
    ULONG DisplayMode;
    BOOLEAN Ready;
} S5_DEVICE_CONTEXT, *PS5_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S5_DEVICE_CONTEXT, S5GetDeviceContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD S5EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE S5EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE S5EvtReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL S5EvtIoDeviceControl;
