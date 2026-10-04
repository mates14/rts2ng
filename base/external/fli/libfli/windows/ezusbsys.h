//////////////////////////////////////////////////////////////////////
//
// File:      ezusbsys.h
// $Archive: /EzUsb/Drivers/ezusbdrv/ezusbsys.h $
//
// Purpose:
//    Header file for the Ezusb USB Device Driver
//
// Environment:
//    kernel mode
//
// $Author: Markm $
//
// $History: ezusbsys.h $           


#ifndef __ezusbsys_h__
#define __ezusbsys_h__

//
// Vendor specific request code for Anchor Upload/Download
//
// This one is implemented in the core
//
#define ANCHOR_LOAD_INTERNAL  0xA0

//
// This command is not implemented in the core.  Requires firmware
//
#define ANCHOR_LOAD_EXTERNAL  0xA3

//
// This is the highest internal RAM address for the AN2131Q
//
#define MAX_INTERNAL_ADDRESS  0x1B3F

#define INTERNAL_RAM(address) ((address <= MAX_INTERNAL_ADDRESS) ? 1 : 0)
//
// EZ-USB Control and Status Register.  Bit 0 controls 8051 reset
//
#define CPUCS_REG    0x7F92


#ifndef _BYTE_DEFINED
#define _BYTE_DEFINED
typedef unsigned char BYTE;
#endif // !_BYTE_DEFINED

#ifndef _WORD_DEFINED
#define _WORD_DEFINED
typedef unsigned short WORD;
#endif // !_WORD_DEFINED

typedef struct _VENDOR_REQUEST_IN
{
    BYTE    bRequest;
    WORD    wValue;
    WORD    wIndex;
    WORD    wLength;
    BYTE    direction;
    BYTE    bData;
} VENDOR_REQUEST_IN, *PVENDOR_REQUEST_IN;


///////////////////////////////////////////////////////////
//
// control structure for Anchor Downloads
//
///////////////////////////////////////////////////////////
typedef struct _ANCHOR_DOWNLOAD_CONTROL
{
   WORD Offset;
} ANCHOR_DOWNLOAD_CONTROL, *PANCHOR_DOWNLOAD_CONTROL;

#define MAX_INTEL_HEX_RECORD_LENGTH 16

typedef struct _INTEL_HEX_RECORD
{
   BYTE  Length;
   WORD  Address;
   BYTE  Type;
   BYTE  Data[MAX_INTEL_HEX_RECORD_LENGTH];
} INTEL_HEX_RECORD, *PINTEL_HEX_RECORD;

typedef struct _SET_INTERFACE_IN
{
   UCHAR interfaceNum;
   UCHAR alternateSetting;
} SET_INTERFACE_IN, *PSET_INTERFACE_IN;

typedef struct _GET_STRING_DESCRIPTOR_IN
{
   UCHAR    Index;
   USHORT   LanguageId;
} GET_STRING_DESCRIPTOR_IN, *PGET_STRING_DESCRIPTOR_IN;

typedef struct _EZUSB_DRIVER_VERSION
{
   WORD     MajorVersion;
   WORD     MinorVersion;
   WORD     BuildVersion;
} EZUSB_DRIVER_VERSION, *PEZUSB_DRIVER_VERSION;

#ifdef DRIVER

typedef struct _RING_BUFFER
{
   PUCHAR      inPtr;
   PUCHAR      outPtr;
   ULONG       totalSize;
   ULONG       currentSize;
   KSPIN_LOCK	spinLock;
   PUCHAR      buffer;
} RING_BUFFER, *PRING_BUFFER;

PRING_BUFFER
AllocRingBuffer(
   ULONG    Size
   );

VOID
FreeRingBuffer(
   PRING_BUFFER   ringBuffer
   );

ULONG
ReadRingBuffer(
   PRING_BUFFER   ringBuffer,
   PUCHAR         readBuffer,
   ULONG          numberOfBytesToRead
   );
   
ULONG
WriteRingBuffer(
   PRING_BUFFER   ringBuffer,
   PUCHAR         writeBuffer,
   ULONG          numberOfBytesToWrite
   );

typedef struct _EZUSB_FIRMWARE
{
   // tag contains a string to identify the start of the firmware
   // image in the driver binary.  Another utilty can then be used
   // to replace the firmware image inthe driver without requiring
   // a recompile
   UCHAR tag[10];
   ULONG size;
   UCHAR firmware[];
} EZUSB_FIRMWARE, *PEZUSB_FIRMWARE;

// 
// this is the default number of IRP's to queue for streaming ISO
// data.
//
#define DEFAULT_ISO_BUFFER_COUNT 2

//
// Default number of frames of ISO data transferred by a single ISO
// URB/IRP
//
#define DEFAULT_ISO_FRAMES_PER_BUFFER 10

typedef struct _ISO_STREAM_OBJECT ISO_STREAM_OBJECT, *PISO_STREAM_OBJECT;

typedef struct _ISO_TRANSFER_OBJECT
{
   ULONG Frame;
   PISO_STREAM_OBJECT StreamObject;
   PURB Urb;
   PIRP Irp;
   KEVENT Done;
} ISO_TRANSFER_OBJECT, *PISO_TRANSFER_OBJECT;

typedef struct _ISO_STREAM_OBJECT
{
   PDEVICE_OBJECT DeviceObject;
   ULONG PacketSize;
   ULONG NumPackets;
   PUSBD_PIPE_INFORMATION PipeInfo;
   PVOID TransferBuffer;
   ULONG TransferBufferLength;
   PVOID IsoDescriptorBuffer;
   ULONG FramesPerBuffer;
   ULONG BufferCount;
   ULONG PendingTransfers;
   PRING_BUFFER DataRingBuffer;
   PRING_BUFFER DescriptorRingBuffer;
   PISO_TRANSFER_OBJECT TransferObject;
} ISO_STREAM_OBJECT, *PISO_STREAM_OBJECT;


#define Ezusb_NAME_MAX  64




//
// This is an unused structure in this driver, but is provided here
// so when you extend the driver to deal with USB pipes, you may wish
// to use this structure as an example or model.
//
typedef struct _EZUSB_PIPE {
   ULONG Mode;
   ULONG Option;
   ULONG Param1;
   ULONG Param2;
   WCHAR Name[Ezusb_NAME_MAX];
   PUSBD_PIPE_INFORMATION PipeInfo;
} EZUSB_PIPE, *PEZUSB_PIPE;


/*
// The interface number on this device that this driver expects to use
// This would be in the bInterfaceNumber field of the Interface Descriptor, hence
// this device driver would need to know this value.
*/
#define SAMPLE_INTERFACE_NBR 0x00


//
// A structure representing the instance information associated with
// this particular device.
//
typedef struct _DEVICE_EXTENSION
{

   // physical device object
   PDEVICE_OBJECT PhysicalDeviceObject;        

   // Device object we call when submitting Urbs/Irps to the USB stack
   PDEVICE_OBJECT		StackDeviceObject;		

   // Indicates that we have recieved a STOP message
   BOOLEAN Stopped;

   // Indicates the device needs to be cleaned up (ie., some configuration
   // has occurred and needs to be torn down).
   BOOLEAN NeedCleanup;

   // configuration handle for the configuration the
   // device is currently in
   USBD_CONFIGURATION_HANDLE ConfigurationHandle;

   // ptr to the USB device descriptor
   // for this device
   PUSB_DEVICE_DESCRIPTOR DeviceDescriptor;

   // we support up to one interface
   PUSBD_INTERFACE_INFORMATION Interface;

   // the number of device handles currently open to the device object.
   // Gets incremented by Create and decremented by Close
   ULONG                OpenHandles;

   // Name buffer for our named Functional device object link
   WCHAR DeviceLinkNameBuffer[Ezusb_NAME_MAX];

   // This member is used to store the URB status of the
   // most recently failed URB.  If a USB transfer fails, a caller
   // can use IOCTL_EZUSB_GET_LAST_ERROR to retrieve this value.
   // There's only room for one, so you better get it quick (or at
   // least before the next URB failure occurs).
   USBD_STATUS LastFailedUrbStatus;

   // use counter for the device.  Gets incremented when the driver receives
   // a request and gets decremented when a request s completed.
   LONG usage;

   // this ev gets set when it is ok to remove the device
	KEVENT evRemove;

   // TRUE if we're trying to remove this device
   BOOLEAN removing;

   BOOLEAN StopIsoStream;

   PRING_BUFFER DataRingBuffer;
   PRING_BUFFER DescriptorRingBuffer;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION;


#if DBG

#define Ezusb_KdPrint(_x_) DbgPrint("Ezusb.SYS: "); \
                             DbgPrint _x_ ;
#define TRAP() DbgBreakPoint()
#else
#define Ezusb_KdPrint(_x_)
#define TRAP()
#endif


NTSTATUS
Ezusb_Dispatch(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    );

VOID
Ezusb_Unload(
    IN PDRIVER_OBJECT DriverObject
    );

NTSTATUS
Ezusb_StartDevice(
    IN  PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
Ezusb_StopDevice(
    IN  PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
Ezusb_RemoveDevice(
    IN  PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
Ezusb_CallUSBD(
    IN PDEVICE_OBJECT DeviceObject,
    IN PURB Urb
    );

NTSTATUS
Ezusb_PnPAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject
    );

NTSTATUS
Ezusb_CreateDeviceObject(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT *DeviceObject,
    LONG Instance
    );

NTSTATUS
Ezusb_ConfigureDevice(
    IN  PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
Ezusb_Create(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    );

NTSTATUS
Ezusb_Close(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    );

NTSTATUS
Ezusb_Read_Write(
   IN  PDEVICE_OBJECT DeviceObject,
   IN  PIRP Irp
   );

NTSTATUS
Ezusb_ProcessIOCTL(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    );

NTSTATUS
Ezusb_SelectInterfaces(
    IN PDEVICE_OBJECT DeviceObject,
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    IN PUSBD_INTERFACE_INFORMATION Interface
    );

NTSTATUS
Ezusb_ResetPipe(
   IN PDEVICE_OBJECT DeviceObject,
   IN ULONG PipeNum
   );

NTSTATUS
Ezusb_AbortPipe(
    IN PDEVICE_OBJECT DeviceObject,
    IN USBD_PIPE_HANDLE PipeHandle
    );

ULONG
Ezusb_GetCurrentFrameNumber(
    IN PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
Ezusb_Read_Write_Direct(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PIRP Irp,
    IN  BOOLEAN Read
    );

ULONG
Ezusb_DownloadTest(
    IN PDEVICE_OBJECT DeviceObject,
    IN PVENDOR_REQUEST_IN pVendorRequest
    );

NTSTATUS
Ezusb_ResetParentPort(
    IN IN PDEVICE_OBJECT DeviceObject
    );

VOID
Ezusb_Cleanup(
    PDEVICE_OBJECT DeviceObject
    );

ULONG
Ezusb_GetDeviceDescriptor(
    IN PDEVICE_OBJECT DeviceObject,
    PVOID             pvOutputBuffer
    );

ULONG
Ezusb_GetConfigDescriptor(
    IN PDEVICE_OBJECT DeviceObject,
    PVOID             pvOutputBuffer,
    ULONG             ulngth
    );

ULONG
Ezusb_VendorRequest(
    IN PDEVICE_OBJECT DeviceObject,
    PVENDOR_REQUEST_IN pVendorRequest
    );

ULONG
Ezusb_GetCurrentConfig(
    IN PDEVICE_OBJECT DeviceObject,
    IN PVENDOR_REQUEST_IN pVendorRequest
    );

ULONG
Ezusb_GetCurrentInterface(
    IN PDEVICE_OBJECT DeviceObject,
    IN PVENDOR_REQUEST_IN pVendorRequest
    );

PUSB_CONFIGURATION_DESCRIPTOR
GetConfigDescriptor(
    IN PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
ConfigureDevice(
    IN  PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
SetInterface(
   IN PDEVICE_OBJECT DeviceObject,
   IN UCHAR InterfaceNumber,
   IN UCHAR AlternateSetting
   );

ULONG
Ezusb_GetStringDescriptor(
    IN PDEVICE_OBJECT DeviceObject,
    UCHAR             Index,
    USHORT            LanguageId,
    PVOID             pvOutputBuffer,
    ULONG             ulLength
    );

NTSTATUS
Ezusb_VendorRequest2(
   IN PDEVICE_OBJECT DeviceObject,
   IN PIRP Irp
   );


NTSTATUS
ForwardAndWait(
   IN PDEVICE_OBJECT DeviceObject,
   IN PIRP Irp
   );


NTSTATUS
Ezusb_DefaultPnpHandler(
   IN PDEVICE_OBJECT fdo,
   IN PIRP Irp
   );


NTSTATUS
Ezusb_DispatchPnp(
   IN PDEVICE_OBJECT DeviceObject,
   IN PIRP           Irp
   );

NTSTATUS
Ezusb_DispatchPower(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    );

NTSTATUS
Ezusb_HandleStartDevice(
   IN PDEVICE_OBJECT DeviceObject,
   IN PIRP Irp
   );

NTSTATUS
Ezusb_HandleRemoveDevice(
   IN PDEVICE_OBJECT fdo,
   IN PIRP Irp
   );

NTSTATUS 
OnRequestComplete(
   IN PDEVICE_OBJECT DeviceObject,
   IN PIRP Irp,
   IN PKEVENT pev
   );

NTSTATUS
CompleteRequest(
   IN PIRP Irp,
   IN NTSTATUS status,
   IN ULONG info
   );

BOOLEAN LockDevice(
   IN PDEVICE_OBJECT fdo
   );

void UnlockDevice(
   PDEVICE_OBJECT fdo
   );


NTSTATUS InitTransferObject(
   IN OUT PISO_STREAM_OBJECT streamObject,
   IN ULONG index
   );

NTSTATUS Ezusb_StartIsoTransfer(
   IN PDEVICE_OBJECT fdo,
   IN PIRP Irp
   );

NTSTATUS IsoTransferComplete(
   IN PDEVICE_OBJECT fdo,
   IN PIRP Irp,
   IN PVOID Context
   );


NTSTATUS Ezusb_AnchorDownload(
   PDEVICE_OBJECT fdo,
   WORD offset,
   PUCHAR downloadBuffer,
   ULONG downloadSize
   );

NTSTATUS Ezusb_DownloadIntelHex(
   PDEVICE_OBJECT fdo,
   PINTEL_HEX_RECORD hexRecord
   );

NTSTATUS Ezusb_8051Reset(
   PDEVICE_OBJECT fdo,
   UCHAR resetBit
   );

NTSTATUS Ezusb_StartIsoStream(
   IN PDEVICE_OBJECT fdo,
   IN PIRP Irp
   );

#endif      //DRIVER section


///////////////////////////////////////////////////////
//
//              IOCTL Definitions
//
// User mode applications wishing to send IOCTLs to a kernel mode driver
// must use this file to set up the correct type of IOCTL code permissions.
//
// Note: this file depends on the file DEVIOCTL.H which contains the macro
// definition for "CTL_CODE" below.  Include that file before  you include
// this one in your source code.
//
///////////////////////////////////////////////////////

//
// Set the base of the IOCTL control codes.  This is somewhat of an
// arbitrary base number, so you can change this if you want unique
// IOCTL codes.  You should consult the Windows NT DDK for valid ranges
// of IOCTL index codes before you choose a base index number.
//

#define Ezusb_IOCTL_INDEX  0x0800


#define IOCTL_Ezusb_GET_PIPE_INFO     CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+0,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_GET_DEVICE_DESCRIPTOR CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+1,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_GET_CONFIGURATION_DESCRIPTOR CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+2,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_BULK_OR_INTERRUPT_WRITE     CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+3,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_BULK_OR_INTERRUPT_READ      CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+4,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_VENDOR_REQUEST              CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+5,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_GET_CURRENT_CONFIG          CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+6,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_ANCHOR_DOWNLOAD             CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+7,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_RESET  CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+12,\
                                                   METHOD_IN_DIRECT,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_RESETPIPE  CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+13,\
                                                   METHOD_IN_DIRECT,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_ABORTPIPE  CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+15,\
                                                   METHOD_IN_DIRECT,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_SETINTERFACE  CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+16,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_Ezusb_GET_STRING_DESCRIPTOR CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+17,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)


//
// Perform an IN transfer over the specified bulk or interrupt pipe.
//
// lpInBuffer: BULK_TRANSFER_CONTROL stucture specifying the pipe number to read from
// nInBufferSize: sizeof(BULK_TRANSFER_CONTROL)
// lpOutBuffer: Buffer to hold data read from the device.  
// nOutputBufferSize: size of lpOutBuffer.  This parameter determines
//    the size of the USB transfer.
// lpBytesReturned: actual number of bytes read
// 
#define IOCTL_BULK_READ             CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+19,\
                                                   METHOD_OUT_DIRECT,  \
                                                   FILE_ANY_ACCESS)

//
// Perform an OUT transfer over the specified bulk or interrupt pipe.
//
// lpInBuffer: BULK_TRANSFER_CONTROL stucture specifying the pipe number to write to
// nInBufferSize: sizeof(BULK_TRANSFER_CONTROL)
// lpOutBuffer: Buffer of data to write to the device
// nOutputBufferSize: size of lpOutBuffer.  This parameter determines
//    the size of the USB transfer.
// lpBytesReturned: actual number of bytes written
// 
#define IOCTL_BULK_WRITE            CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+20,\
                                                   METHOD_IN_DIRECT,  \
                                                   FILE_ANY_ACCESS)

//
// The following IOCTL's are defined as using METHOD_DIRECT_IN buffering.
// This means that the output buffer is directly mapped into system
// space and probed for read access by the driver.  This means that it is
// brought into memory if it happens to be paged out to disk.  Even though
// the buffer is only probed for read access, it is safe (probably) to
// write to it as well.  This read/write capability is used for the loopback
// IOCTL's
// 

// TODO Insert Loopback IOCTL's

//
// Retrieve the current USB frame number from the Host Controller
//
// lpInBuffer: NULL
// nInBufferSize: 0
// lpOutBuffer: PULONG to hold current frame number
// nOutputBufferSize: sizeof(PULONG)
// 
#define IOCTL_EZUSB_GET_CURRENT_FRAME_NUMBER  CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+21,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)


//
// Performs a vendor or class specific control transfer to EP0.  The contents of
// the input parameter determine the type of request.  See the USB spec
// for more information on class and vendor control transfers.
//
// lpInBuffer: PVENDOR_OR_CLASS_REQUEST_CONTROL
// nInBufferSize: sizeof(VENDOR_OR_CLASS_REQUEST_CONTROL)
// lpOutBuffer: pointer to a buffer if the request involves a data transfer
// nOutputBufferSize: size of the transfer buffer (corresponds to the wLength
//    field of the USB setup packet)
// 
#define IOCTL_EZUSB_VENDOR_OR_CLASS_REQUEST   CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+22,\
                                                   METHOD_IN_DIRECT,  \
                                                   FILE_ANY_ACCESS)

//
// Retrieves the actual USBD_STATUS code for the most recently failed
// URB.
//
// lpInBuffer: NULL
// nInBufferSize: 0
// lpOutBuffer: PULONG to hold the URB status
// nOutputBufferSize: sizeof(ULONG)
// 

#define IOCTL_GET_LAST_USBD_ERROR   CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+23,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

//
// Reads from the specified ISO endpoint. (USB IN Transfer)
//
// lpInBuffer: ISO_TRANSFER_CONTROL
// nInBufferSize: sizeof(ISO_TRANSFER_CONTROL)
// lpOutBuffer: buffer to hold data read from the device
// nOutputBufferSize: size of the read buffer.
//
// 
// 

#define IOCTL_EZUSB_ISO_READ          CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+25,\
                                                   METHOD_OUT_DIRECT,  \
                                                   FILE_ANY_ACCESS)

//
// Writes to the specified ISO endpoint. (USB OUT Transfer)
//
// lpInBuffer: ISO_TRANSFER_CONTROL
// nInBufferSize: sizeof(ISO_TRANSFER_CONTROL)
// lpOutBuffer: buffer to hold data to write to the device
// nOutputBufferSize: size of the write buffer.
//
// 
// 

#define IOCTL_EZUSB_ISO_WRITE          CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+26,\
                                                   METHOD_IN_DIRECT,  \
                                                   FILE_ANY_ACCESS)

//
// Performs and Anchor Download.
//
// lpInBuffer: PANCHOR_DOWNLOAD_CONTROL
// nInBufferSize: sizeof(ANCHOR_DOWNLOAD_CONTROL)
// lpOutBuffer: pointer to a buffer of data to download to the device
// nOutputBufferSize: size of the transfer buffer
// 
#define IOCTL_EZUSB_ANCHOR_DOWNLOAD   CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+27,\
                                                   METHOD_IN_DIRECT,  \
                                                   FILE_ANY_ACCESS)

//
// Returns driver version information
//
// lpInBuffer: NULL
// nInBufferSize: 0
// lpOutBuffer: PEZUSB_DRIVER_VERSION
// nOutputBufferSize: sizeof(EZUSB_DRIVER_VERSION)
// 
#define IOCTL_EZUSB_GET_DRIVER_VERSION   CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+29,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_EZUSB_START_ISO_STREAM     CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+30,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_EZUSB_STOP_ISO_STREAM     CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+31,\
                                                   METHOD_BUFFERED,  \
                                                   FILE_ANY_ACCESS)

#define IOCTL_EZUSB_READ_ISO_BUFFER     CTL_CODE(FILE_DEVICE_UNKNOWN,  \
                                                   Ezusb_IOCTL_INDEX+32,\
                                                   METHOD_OUT_DIRECT,  \
                                                   FILE_ANY_ACCESS)

#endif /* __ezusbsys_h__ */