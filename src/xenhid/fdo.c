/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */ 

#define INITGUID
#include <ntddk.h>
#include <procgrp.h>
#include <ntstrsafe.h>
#include <hidport.h>

#include <hid_interface.h>

#include "fdo.h"
#include "driver.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

struct _XENHID_FDO {
    PDEVICE_OBJECT          DeviceObject;
    PDEVICE_OBJECT          LowerDeviceObject;
    BOOLEAN                 Enabled;
    BOOLEAN                 Running;

    XENHID_HID_INTERFACE    HidInterface;
    IO_CSQ                  IrpQueue;
    KSPIN_LOCK              IrpLock;
    LIST_ENTRY              IrpList;
};

ULONG
FdoGetSize(
    VOID
    )
{
    return sizeof(XENHID_FDO);
}

VOID
FdoCsqInsertIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, IrpQueue);

    InsertTailList(&Fdo->IrpList, &Irp->Tail.Overlay.ListEntry);
}

VOID
FdoCsqRemoveIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, IrpQueue);

    UNREFERENCED_PARAMETER(Fdo);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

PIRP
FdoCsqPeekNextIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp,
    IN  PVOID   Context
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, IrpQueue);
    PLIST_ENTRY ListEntry;
    PIRP        NextIrp;

    ListEntry = RemoveHeadList(&Fdo->IrpList);
    if (ListEntry == &Fdo->IrpList)
        return NULL;

    NextIrp = CONTAINING_RECORD(ListEntry, IRP, Tail.Overlay.ListEntry);
    return NextIrp;
}

VOID
FdoCsqAcquireLock(
    IN  PIO_CSQ Csq,
    OUT PKIRQL  Irql
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, IrpQueue);

    KeAcquireSpinLock(&Fdo->IrpLock, Irql);
}

VOID
FdoCsqReleaseLock(
    IN  PIO_CSQ Csq,
    IN  KIRQL   Irql
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, IrpQueue);

    KeReleaseSpinLock(&Fdo->IrpLock, Irql);
}

VOID
FdoCsqCompleteCanceledIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, IrpQueue);

    UNREFERENCED_PARAMETER(Fdo);
    Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static DECLSPEC_NOINLINE BOOLEAN
FdoHidCallback(
    IN  PVOID       Argument,
    IN  PVOID       Buffer,
    IN  ULONG       Length
    )
{
    PXENHID_FDO     Fdo = Argument;
    PIRP            Irp;

    Irp = IoCsqRemoveNextIrp(&Fdo->IrpQueue, NULL);
    if (Irp == NULL)
        return FALSE;

    Trace("=====>\n");

    RtlCopyMemory(Irp->UserBuffer,
                  Buffer,
                  Length);
    Irp->IoStatus.Information = Length;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<=====\n");
    return TRUE;
}

static DECLSPEC_NOINLINE VOID
FdoResume(
    IN  PXENHID_FDO Fdo
    )
{
    Trace("=====>\n");
    if (Fdo->Running)
        goto done;

    Fdo->Running = TRUE;
done:
    Trace("<=====\n");
}

static DECLSPEC_NOINLINE VOID
FdoSuspend(
    IN  PXENHID_FDO Fdo
    )
{
    Trace("=====>\n");
    if (Fdo->Running)
        goto done;

    for (;;) {
        PIRP        Irp;

        Irp = IoCsqRemoveNextIrp(&Fdo->IrpQueue, NULL);
        if (Irp == NULL)
            break;

        Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    Fdo->Running = FALSE;
done:
    Trace("<=====\n");
}

static DECLSPEC_NOINLINE NTSTATUS
FdoD3ToD0(
    IN  PXENHID_FDO Fdo
    )
{
    NTSTATUS        status;

    Trace("=====>\n");
    if (Fdo->Enabled)
        goto done;

    status = XENHID_HID(Acquire,
                        &Fdo->HidInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENHID_HID(Enable,
                        &Fdo->HidInterface,
                        FdoHidCallback,
                        Fdo);
    if (!NT_SUCCESS(status))
        goto fail2;

    Fdo->Enabled = TRUE;
done:
    Trace("<=====\n");
    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    XENHID_HID(Release,
               &Fdo->HidInterface);

fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static DECLSPEC_NOINLINE VOID
FdoD0ToD3(
    IN  PXENHID_FDO Fdo
    )
{
    Trace("=====>\n");
    if (!Fdo->Enabled)
        goto done;

    XENHID_HID(Disable,
               &Fdo->HidInterface);

    XENHID_HID(Release,
               &Fdo->HidInterface);

    Fdo->Enabled = FALSE;
done:
    Trace("<=====\n");
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchDefault(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoForwardIrpSynchronously(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
FdoForwardIrpSynchronously(
    IN  PXENHID_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    KEVENT              Event;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoForwardIrpSynchronously,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = Irp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, Irp->IoStatus.Status);
    }

    Trace("%08x\n", status);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPnp(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    status = STATUS_SUCCESS;

    switch (StackLocation->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        if (!NT_SUCCESS(status))
            return status;
        status = FdoD3ToD0(Fdo);
        return status;

    case IRP_MN_REMOVE_DEVICE:
        FdoD0ToD3(Fdo);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        FdoSuspend(Fdo);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        FdoResume(Fdo);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        FdoSuspend(Fdo);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        FdoResume(Fdo);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        FdoD0ToD3(Fdo);
        break;

    default:
        status = Irp->IoStatus.Status;
        break;
    }

    Irp->IoStatus.Status = status;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPower(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    switch (StackLocation->MinorFunction) {
    case IRP_MN_SET_POWER:
        if (StackLocation->Parameters.Power.Type != DevicePowerState)
            break;

        if (StackLocation->Parameters.Power.State.DeviceState == PowerDeviceD0)
            FdoD3ToD0(Fdo);
        else
            FdoD0ToD3(Fdo);

        break;

    default:
        break;
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    status = PoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchInternal(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Type3Input;
    ULONG               IoControlCode;
    ULONG               InputLength;
    ULONG               OutputLength;
    PVOID               Buffer;
    ULONG               Returned;
    PHID_XFER_PACKET    Packet;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    IoControlCode = StackLocation->Parameters.DeviceIoControl.IoControlCode;
    Type3Input = (ULONG)(ULONG_PTR)StackLocation->Parameters.DeviceIoControl.Type3InputBuffer;
    InputLength = StackLocation->Parameters.DeviceIoControl.InputBufferLength;
    OutputLength = StackLocation->Parameters.DeviceIoControl.OutputBufferLength;
    Buffer = Irp->UserBuffer;
    Packet = Irp->UserBuffer;

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = XENHID_HID(GetDeviceAttributes,
                            &Fdo->HidInterface,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = XENHID_HID(GetDeviceDescriptor,
                            &Fdo->HidInterface,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = XENHID_HID(GetReportDescriptor,
                            &Fdo->HidInterface,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_STRING:
        status = XENHID_HID(GetString,
                            &Fdo->HidInterface,
                            Type3Input,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_INDEXED_STRING:
        status = XENHID_HID(GetIndexedString,
                            &Fdo->HidInterface,
                            Type3Input,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;

        break;

    case IOCTL_HID_GET_FEATURE:
        status = XENHID_HID(GetFeature,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_SET_FEATURE:
        status = XENHID_HID(SetFeature,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen);
        break;

    case IOCTL_HID_GET_INPUT_REPORT:
        status = XENHID_HID(GetInputReport,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_SET_OUTPUT_REPORT:
        status = XENHID_HID(SetOutputReport,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen);
        break;

    case IOCTL_HID_READ_REPORT:
        status = STATUS_PENDING;
        IoMarkIrpPending(Irp);
        IoCsqInsertIrp(&Fdo->IrpQueue, Irp, NULL);
        XENHID_HID(ReadReport,
                   &Fdo->HidInterface);
        break;

    case IOCTL_HID_WRITE_REPORT:
        status = XENHID_HID(WriteReport,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen);
        break;

    // Other HID IOCTLs are failed as not supported
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (status != STATUS_PENDING) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    return status;
}

NTSTATUS
FdoDispatch(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    switch (StackLocation->MajorFunction) {
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        status = FdoDispatchInternal(Fdo, Irp);
        break;

    case IRP_MJ_PNP:
        status = FdoDispatchPnp(Fdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = FdoDispatchPower(Fdo, Irp);
        break;

    default:
        status = FdoDispatchDefault(Fdo, Irp);
        break;
    }

    return status;
}

static FORCEINLINE NTSTATUS
FdoQueryHidInterface(
    IN  PXENHID_FDO     Fdo
    )
{
    KEVENT              Event;
    IO_STATUS_BLOCK     StatusBlock;
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_INTERFACE;

    StackLocation->Parameters.QueryInterface.InterfaceType = &GUID_XENHID_HID_INTERFACE;
    StackLocation->Parameters.QueryInterface.Size = sizeof (XENHID_HID_INTERFACE);
    StackLocation->Parameters.QueryInterface.Version = XENHID_HID_INTERFACE_VERSION_MAX;
    StackLocation->Parameters.QueryInterface.Interface = (PINTERFACE)&Fdo->HidInterface;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);
    return status;
}

NTSTATUS
FdoCreate(
    IN  PXENHID_FDO     Fdo,
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PDEVICE_OBJECT  LowerDeviceObject
    )
{
    NTSTATUS            status;

    Trace("=====>\n");

    Fdo->DeviceObject = DeviceObject;
    Fdo->LowerDeviceObject = LowerDeviceObject;

    InitializeListHead(&Fdo->IrpList);
    KeInitializeSpinLock(&Fdo->IrpLock);

    status = IoCsqInitialize(&Fdo->IrpQueue,
                             FdoCsqInsertIrp,
                             FdoCsqRemoveIrp,
                             FdoCsqPeekNextIrp,
                             FdoCsqAcquireLock,
                             FdoCsqReleaseLock,
                             FdoCsqCompleteCanceledIrp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = FdoQueryHidInterface(Fdo);
    if (!NT_SUCCESS(status))
        goto fail2;

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    RtlZeroMemory(&Fdo->IrpQueue, sizeof(IO_CSQ));

fail1:
    Error("fail1 %08x\n", status);

    RtlZeroMemory(&Fdo->IrpList, sizeof(LIST_ENTRY));
    RtlZeroMemory(&Fdo->IrpLock, sizeof(KSPIN_LOCK));

    Fdo->DeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof(XENHID_FDO)));
    return status;
}

VOID
FdoDestroy(
    IN  PXENHID_FDO Fdo
    )
{
    Trace("=====>\n");

    RtlZeroMemory(&Fdo->HidInterface,
                  sizeof(XENHID_HID_INTERFACE));
    RtlZeroMemory(&Fdo->IrpQueue, sizeof(IO_CSQ));
    RtlZeroMemory(&Fdo->IrpList, sizeof(LIST_ENTRY));
    RtlZeroMemory(&Fdo->IrpLock, sizeof(KSPIN_LOCK));

    Fdo->DeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof(XENHID_FDO)));
    Trace("<=====\n");
}
