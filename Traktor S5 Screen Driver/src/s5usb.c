#define INITGUID
#include "s5usb.h"

static NTSTATUS
S5FindBulkOut(_In_ WDFUSBINTERFACE UsbInterface,
              _Out_ WDFUSBPIPE* BulkOut)
{
    const UCHAR count = WdfUsbInterfaceGetNumConfiguredPipes(UsbInterface);
    for (UCHAR index = 0; index < count; ++index) {
        WDF_USB_PIPE_INFORMATION info;
        WDF_USB_PIPE_INFORMATION_INIT(&info);
        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(UsbInterface, index, &info);
        if (pipe == NULL) {
            continue;
        }
        if (info.PipeType == WdfUsbPipeTypeBulk &&
            info.EndpointAddress == S5_USB_BULK_OUT) {
            *BulkOut = pipe;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS
S5CreateDeviceInterfaces(_In_ WDFDEVICE Device)
{
    // C757 is the interface observed in live HWS discovery. Keep the
    // production interface surface to that verified GUID.
    NTSTATUS status = WdfDeviceCreateDeviceInterface(
        Device, &GUID_DEVINTERFACE_NI_HWS_DISPLAY, NULL);
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION) {
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject,
            _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, S5EvtDeviceAdd);
    config.DriverPoolTag = '5SFA';
    NTSTATUS status = WdfDriverCreate(DriverObject, RegistryPath,
                                      WDF_NO_OBJECT_ATTRIBUTES, &config,
                                      WDF_NO_HANDLE);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "s5usb: WdfDriverCreate status=%08X\n", status);
    return status;
}

NTSTATUS
S5EvtDeviceAdd(_In_ WDFDRIVER Driver,
               _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    // Keep the WDF device type in the documented range. The vendor value
    // 0x8089 remains part of the IOCTL code, but is not used as the WDF
    // object type during PnP creation.
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    // The USB target is created during PrepareHardware. Registering these
    // callbacks is required; without it KMDF would create the device object
    // but never select the USB configuration or discover endpoint 0x03.
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = S5EvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = S5EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, S5_DEVICE_CONTEXT);
    // WdfUsbTargetPipeWriteSynchronously requires PASSIVE_LEVEL. Propagate
    // this execution constraint to the device's sequential IOCTL queue.
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PS5_DEVICE_CONTEXT context = S5GetDeviceContext(device);
    RtlZeroMemory(context, sizeof(*context));

    status = S5CreateDeviceInterfaces(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = S5EvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    return WdfIoQueueCreate(device, &queueConfig, &queueAttributes, &context->IoQueue);
}

NTSTATUS
S5EvtPrepareHardware(_In_ WDFDEVICE device,
                     _In_ WDFCMRESLIST ResourcesRaw,
                     _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PS5_DEVICE_CONTEXT context = S5GetDeviceContext(device);
    context->Ready = FALSE;
    WDF_USB_DEVICE_CREATE_CONFIG createConfig;
    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createConfig, USBD_CLIENT_CONTRACT_VERSION_602);

    NTSTATUS status = WdfUsbTargetDeviceCreateWithParameters(
        device, &createConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->UsbDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selectConfig;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&selectConfig);
    status = WdfUsbTargetDeviceSelectConfig(context->UsbDevice,
                                            WDF_NO_OBJECT_ATTRIBUTES,
                                            &selectConfig);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context->UsbInterface = selectConfig.Types.SingleInterface.ConfiguredUsbInterface;
    context->InterfaceNumber = WdfUsbInterfaceGetInterfaceNumber(context->UsbInterface);

    // The S5 display function is vendor-specific FF/BD. The composite USB
    // stack gives this PDO the MI_04 interface, so a single-interface config is
    // expected here. Reject an accidental bind to an audio/HID function.
    USB_INTERFACE_DESCRIPTOR descriptor;
    RtlZeroMemory(&descriptor, sizeof(descriptor));
    WdfUsbInterfaceGetDescriptor(context->UsbInterface, 0, &descriptor);
    if (descriptor.bInterfaceNumber != 4 ||
        descriptor.bInterfaceClass != S5_USB_INTERFACE_CLASS ||
        descriptor.bInterfaceSubClass != S5_USB_INTERFACE_SUBCLASS) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    status = S5FindBulkOut(context->UsbInterface, &context->BulkOut);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(context->BulkOut);
    context->DisplayMode = S5_DISPLAY_MODE;
    context->Ready = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
S5EvtReleaseHardware(_In_ WDFDEVICE device,
                     _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PS5_DEVICE_CONTEXT context = S5GetDeviceContext(device);
    context->Ready = FALSE;
    context->DisplayMode = 0;
    context->BulkOut = NULL;
    context->UsbInterface = NULL;
    // ReleaseHardware also runs on stop/restart. Destroy the WDF USB target
    // so the next PrepareHardware does not leak a second target and its pipes.
    if (context->UsbDevice != NULL) {
        WdfObjectDelete(context->UsbDevice);
    }
    context->UsbDevice = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS
S5CompleteQuery(_In_ WDFREQUEST request, _In_ ULONG mode)
{
    PVOID buffer = NULL;
    size_t length = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, sizeof(ULONG),
                                                     &buffer, &length);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return status;
    }
    *(UNALIGNED ULONG*)buffer = mode;
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(ULONG));
    return STATUS_SUCCESS;
}

static VOID
S5CompleteDeviceDescriptor(_In_ PS5_DEVICE_CONTEXT context,
                           _In_ WDFREQUEST request,
                           _In_ size_t inputBufferLength)
{
    // Return the connected device's identity, not a synthetic descriptor or
    // a hardcoded serial. HWS associates the display with its HID controller
    // using this serial. The physical S5 has manufacturer=1, product=3,
    // serial=2; swapping these indices prevents that association.
    // The request format observed in the x64 service is:
    //   [0..3] reserved, [4] descriptor type, [5] descriptor index,
    //   [6..7] language ID (little endian).
    if (!context->Ready || context->UsbDevice == NULL) {
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
        return;
    }
    UCHAR descriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
    UCHAR descriptorIndex = 0;
    USHORT language = 0;
    if (inputBufferLength != 0) {
        if (inputBufferLength < 8) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
            return;
        }
        PVOID input = NULL;
        size_t inputLength = 0;
        NTSTATUS inputStatus = WdfRequestRetrieveInputBuffer(
            request, 8, &input, &inputLength);
        if (!NT_SUCCESS(inputStatus)) {
            WdfRequestComplete(request, inputStatus);
            return;
        }
        const UCHAR* requestBytes = (const UCHAR*)input;
        descriptorType = requestBytes[4];
        descriptorIndex = requestBytes[5];
        language = (USHORT)requestBytes[6] |
                   ((USHORT)requestBytes[7] << 8);
    }

    if (descriptorType != USB_DEVICE_DESCRIPTOR_TYPE &&
        descriptorType != USB_STRING_DESCRIPTOR_TYPE) {
        WdfRequestComplete(request, STATUS_NOT_SUPPORTED);
        return;
    }

    PVOID output = NULL;
    size_t length = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, 2, &output, &length);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    if (descriptorType == USB_DEVICE_DESCRIPTOR_TYPE) {
        if (descriptorIndex != 0) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
            return;
        }
        // USBCCGP may replace iProduct with this function's iInterface index.
        // Preserve that valid per-function descriptor; on this S5 it refers
        // to string 28, "Traktor Kontrol S5 BD". Serial remains index 2.
        USB_DEVICE_DESCRIPTOR descriptor;
        WdfUsbTargetDeviceGetDeviceDescriptor(context->UsbDevice, &descriptor);
        const size_t copied = min(length, sizeof(descriptor));
        RtlCopyMemory(output, &descriptor, copied);
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, copied);
        return;
    }

    // Read-only standard GET_DESCRIPTOR on endpoint zero. Copy out only the
    // bytes actually returned by the S5, including the USB descriptor header.
    UCHAR descriptor[256] = {0};
    const ULONG requested = (ULONG)min(length, sizeof(descriptor));
    WDF_USB_CONTROL_SETUP_PACKET setup;
    RtlZeroMemory(&setup, sizeof(setup));
    setup.Packet.bm.Request.Dir = BmRequestDeviceToHost;
    setup.Packet.bm.Request.Type = BmRequestStandard;
    setup.Packet.bm.Request.Recipient = BmRequestToDevice;
    setup.Packet.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup.Packet.wValue.Value = ((USHORT)descriptorType << 8) | descriptorIndex;
    setup.Packet.wIndex.Value = language;
    WDF_MEMORY_DESCRIPTOR memory;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, descriptor, requested);
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(2000));
    ULONG transferred = 0;
    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        context->UsbDevice, NULL, &options, &setup, &memory, &transferred);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    if (transferred > requested) {
        WdfRequestComplete(request, STATUS_DEVICE_DATA_ERROR);
        return;
    }
    RtlCopyMemory(output, descriptor, transferred);
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, transferred);
}

VOID
S5EvtIoDeviceControl(_In_ WDFQUEUE queue,
                     _In_ WDFREQUEST request,
                     _In_ size_t OutputBufferLength,
                     _In_ size_t InputBufferLength,
                     _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    PS5_DEVICE_CONTEXT context = S5GetDeviceContext(device);

    if (IoControlCode == IOCTL_S5_QUERY_STATUS || IoControlCode == 0x80892014) {
        // Preserve the 0.7 prototype's response for 0x80892014 during this
        // source cleanup. The original request is a control transfer, not a
        // mode query; that path is NOT a verified part of screen support.
        (void)S5CompleteQuery(request, context->Ready ? context->DisplayMode : 0);
        return;
    }

    if (IoControlCode == 0x80892004) {
        S5CompleteDeviceDescriptor(context, request, InputBufferLength);
        return;
    }

    // Unverified prototype response: static HWS analysis found this IOCTL,
    // but it was not observed in the failing discovery trace. It must not be
    // treated as an established device-info contract before release.
    if (IoControlCode == 0x80892018) {
        PVOID output = NULL;
        size_t length = 0;
        NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, 15, &output, &length);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(request, status);
            return;
        }
        RtlZeroMemory(output, 15);
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 15);
        return;
    }

    // The original 0x80892804 path returns a 12-byte USB/display capability
    // record. Keep the observed bytes so HWS can complete its handshake while
    // the remaining fields are still being validated against a native trace.
    if (IoControlCode == 0x80892804) {
        PVOID output = NULL;
        size_t length = 0;
        NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, 12, &output, &length);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(request, status);
            return;
        }
        RtlZeroMemory(output, 12);
        ((UCHAR*)output)[0] = 2;
        ((UCHAR*)output)[2] = 4;
        ((UCHAR*)output)[3] = 2;
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 12);
        return;
    }

    if (IoControlCode == IOCTL_S5_WRITE_BULK) {
        if (!context->Ready || context->BulkOut == NULL) {
            WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
            return;
        }
        if (InputBufferLength < 4 || InputBufferLength > S5_MAX_FRAME_SIZE) {
            WdfRequestComplete(request, STATUS_INVALID_BUFFER_SIZE);
            return;
        }

        WDFMEMORY memory;
        NTSTATUS status = WdfRequestRetrieveInputMemory(request, &memory);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(request, status);
            return;
        }
        WDF_MEMORY_DESCRIPTOR descriptor;
        WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&descriptor, memory, NULL);
        WDF_REQUEST_SEND_OPTIONS options;
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(2000));
        ULONG written = 0;
        status = WdfUsbTargetPipeWriteSynchronously(context->BulkOut, NULL,
                                                    &options, &descriptor, &written);
        WdfRequestCompleteWithInformation(request, status, written);
        return;
    }

    WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
}
