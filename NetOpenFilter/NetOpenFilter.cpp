#include <initguid.h>
#include <fltKernel.h>
#include <ntstrsafe.h>

EXTERN_C DRIVER_INITIALIZE DriverEntry;

__declspec(code_seg("PAGE")) NTSTATUS FLTAPI FilterUnload(_In_ FLT_FILTER_UNLOAD_FLAGS Flags);
__declspec(code_seg("PAGE")) NTSTATUS FLTAPI InstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects, _In_ FLT_INSTANCE_SETUP_FLAGS Flags, _In_ DEVICE_TYPE VolumeDeviceType, _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType);
__declspec(code_seg("PAGE")) NTSTATUS FLTAPI InstanceQueryTeardown(_In_ PCFLT_RELATED_OBJECTS FltObjects, _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags);

__declspec(code_seg("PAGE")) FLT_PREOP_CALLBACK_STATUS FLTAPI PreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID *CompletionContext);

__declspec(code_seg("PAGE")) FLT_POSTOP_CALLBACK_STATUS FLTAPI PostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags);


#pragma alloc_text(INIT, DriverEntry)

PFLT_FILTER Filter = nullptr;

constexpr ULONG NET_OPEN_INSTANCE_CONTEXT_POOL_TAG = 'cIoN';
#if DBG
constexpr ULONG NET_OPEN_VOLUME_NAME_BUFFER_POOL_TAG = 'nVoN';
#endif
constexpr ULONG NET_OPEN_MUP_PROVIDER_INFO_POOL_TAG = 'iPoN';

typedef struct _NET_OPEN_INSTANCE_CONTEXT {
    BOOLEAN NetFs;
} NET_OPEN_INSTANCE_CONTEXT, *PNET_OPEN_INSTANCE_CONTEXT;

__declspec(allocate("INIT")) const FLT_CONTEXT_REGISTRATION ContextRegistration[] = {
    { FLT_INSTANCE_CONTEXT, 0, nullptr, sizeof(NET_OPEN_INSTANCE_CONTEXT), NET_OPEN_INSTANCE_CONTEXT_POOL_TAG, nullptr, nullptr, nullptr },
    { FLT_CONTEXT_END }
};

__declspec(allocate("INIT")) const FLT_OPERATION_REGISTRATION OperationRegistration[] = {
    { IRP_MJ_CREATE, 0, PreCreate, PostCreate },
    { IRP_MJ_OPERATION_END }
};

__declspec(allocate("INIT")) const FLT_REGISTRATION FilterRegistration = {
    sizeof(FLT_REGISTRATION), // Size
    FLT_REGISTRATION_VERSION, // Version
    0, // Flags
    ContextRegistration, // ContextRegistration
    OperationRegistration, // OperationRegistration
    FilterUnload, // FilterUnload
    InstanceSetup, // InstanceSetup
    InstanceQueryTeardown, // InstanceQueryTeardown
    nullptr, // InstanceTeardownStart
    nullptr, // InstanceTeardownComplete
    nullptr, // GenerateFileName
    nullptr, // NormalizeNameComponent
    nullptr, // NormalizeContextCleanup
    nullptr, // TransactionNotification
    nullptr, // NormalizeNameComponentEx
};

static UNICODE_STRING PIPE_SHARE_NAME = RTL_CONSTANT_STRING(L"pipe");
static UNICODE_STRING MAILSLOT_SHARE_NAME = RTL_CONSTANT_STRING(L"mailslot");

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(RegistryPath);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "DriverEntry (DriverObject = 0x%p, RegistryPath = %wZ).\n", DriverObject, RegistryPath));

    NTSTATUS status = STATUS_SUCCESS;
    __try {
        status = FltRegisterFilter(DriverObject, &FilterRegistration, &Filter);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltRegisterFilter failed with status 0x%08x.\n", status));
            __leave;
        }

        status = FltStartFiltering(Filter);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltStartFiltering failed with status 0x%08x.\n", status));
            __leave;
        }
    } __finally {
        if (!NT_SUCCESS(status)) {
            if (Filter) {
                FltUnregisterFilter(Filter);
            }
        }
    }

    return status;
}

__declspec(code_seg("PAGE")) NTSTATUS FLTAPI FilterUnload(_In_ FLT_FILTER_UNLOAD_FLAGS Flags)
{
    PAGED_CODE();

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "FilterUnload (Flags = 0x%lx).\n", Flags));

    if (Filter) {
        FltUnregisterFilter(Filter);
    }

    return STATUS_SUCCESS;
}

__declspec(code_seg("PAGE")) NTSTATUS FLTAPI InstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects, _In_ FLT_INSTANCE_SETUP_FLAGS Flags, _In_ DEVICE_TYPE VolumeDeviceType, _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(VolumeFilesystemType);
    UNREFERENCED_PARAMETER(Flags);

    NTSTATUS status = STATUS_SUCCESS;
    PNET_OPEN_INSTANCE_CONTEXT ctx = nullptr;
#if DBG
    PWCH volumeNameBuffer = nullptr;
#endif
    __try {
#if DBG
        ULONG volumeBufferSizeNeeded = 0;
        status = FltGetVolumeName(FltObjects->Volume, nullptr, &volumeBufferSizeNeeded);
        if (status != STATUS_BUFFER_TOO_SMALL) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "1st FltGetVolumeName failed with status 0x%08x.\n", status));
            __leave;
        }

        if (volumeBufferSizeNeeded > MAXUSHORT) {
            __leave;
        }

        volumeNameBuffer = static_cast<PWCH>(ExAllocatePoolWithTag(PagedPool, volumeBufferSizeNeeded, NET_OPEN_VOLUME_NAME_BUFFER_POOL_TAG));
        if (!volumeNameBuffer) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Failed allocating memory for volume name buffer.\n"));
            __leave;
        }

        RtlZeroMemory(volumeNameBuffer, volumeBufferSizeNeeded);

        UNICODE_STRING volumeName{};
        RtlInitEmptyUnicodeString(&volumeName, volumeNameBuffer, static_cast<USHORT>(volumeBufferSizeNeeded));

        status = FltGetVolumeName(FltObjects->Volume, &volumeName, &volumeBufferSizeNeeded);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "2nd FltGetVolumeName failed with status 0x%08x.\n", status));
            __leave;
        }

        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "InstanceSetup (Volume = %wZ).\n", &volumeName));
#endif
        status = FltAllocateContext(
            FltObjects->Filter, FLT_INSTANCE_CONTEXT, sizeof(NET_OPEN_INSTANCE_CONTEXT), PagedPool, reinterpret_cast<PFLT_CONTEXT*>(&ctx));
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltAllocateContext failed with status 0x%08x.\n", status));
            __leave;
        }

        RtlZeroMemory(ctx, sizeof(NET_OPEN_INSTANCE_CONTEXT));

        if (VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM) {
            ctx->NetFs = TRUE;
        } else {
            ctx->NetFs = FALSE;
        }

        status = FltSetInstanceContext(FltObjects->Instance, FLT_SET_CONTEXT_KEEP_IF_EXISTS, ctx, nullptr);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltSetInstanceContext failed with status 0x%08x.\n", status));
        }
    } __finally {
        if (ctx) {
            FltReleaseContext(ctx);
        }
#if DBG
        if (volumeNameBuffer) {
            ExFreePoolWithTag(volumeNameBuffer, NET_OPEN_VOLUME_NAME_BUFFER_POOL_TAG);
        }
#endif
    }

    return status;
}

__declspec(code_seg("PAGE")) NTSTATUS FLTAPI InstanceQueryTeardown(_In_ PCFLT_RELATED_OBJECTS FltObjects, _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "InstanceQueryTeardown (Flags = 0x%lx).\n", Flags));

    return STATUS_SUCCESS;
}

__declspec(code_seg("PAGE")) FLT_PREOP_CALLBACK_STATUS FLTAPI PreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID *CompletionContext)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(FltObjects);
    
    *CompletionContext = nullptr;

    FLT_PREOP_CALLBACK_STATUS callbackStatus = FLT_PREOP_SUCCESS_WITH_CALLBACK;

    if (BooleanFlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION)) {
        callbackStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    return callbackStatus;
}

__declspec(code_seg("PAGE")) FLT_POSTOP_CALLBACK_STATUS FLTAPI PostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(CompletionContext);

    PNET_OPEN_INSTANCE_CONTEXT ctx = nullptr;
    PNETWORK_OPEN_ECP_CONTEXT netOpenEcpCtx = nullptr;
    PFLT_FILE_NAME_INFORMATION fileNameInfo = nullptr;
    NTSTATUS status = STATUS_SUCCESS;
    __try {
        if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING)) {
            __leave;
        }

        NT_ASSERT(BooleanFlagOn(Data->Flags, FLTFL_CALLBACK_DATA_IRP_OPERATION));

        if (!NT_SUCCESS(Data->IoStatus.Status)) {
            __leave;
        }

        status = FltGetInstanceContext(FltObjects->Instance, reinterpret_cast<PFLT_CONTEXT*>(&ctx));
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltGetInstanceContext failed with status 0x%08x.\n", status));
            __leave;
        }

        if (!ctx->NetFs) {
            __leave;
        }

        if (FltObjects->FileObject->FileName.Length == 0) {
            __leave;
        }

        UNICODE_STRING serverName{};
        UNICODE_STRING remainingName{};
        FsRtlDissectName(FltObjects->FileObject->FileName, &serverName, &remainingName);
        UNICODE_STRING shareName{};
        UNICODE_STRING remainingName2{};
        FsRtlDissectName(remainingName, &shareName, &remainingName2);

        if (RtlCompareUnicodeString(&shareName, &PIPE_SHARE_NAME, TRUE) == 0) {
            __leave;
        }

        if (RtlCompareUnicodeString(&shareName, &MAILSLOT_SHARE_NAME, TRUE) == 0) {
            __leave;
        }

        status = FltGetFileNameInformation(Data, FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT, &fileNameInfo);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltGetFileNameInformation failed with status 0x%08x.\n", status));
            __leave;
        }

        status = FltParseFileNameInformation(fileNameInfo);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltParseFileNameInformation failed with status 0x%08x.\n", status));
            __leave;
        }

        PECP_LIST ecpList = nullptr;
        status = FltGetEcpListFromCallbackData(FltObjects->Filter, Data, &ecpList);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FltGetEcpListFromCallbackData failed with status 0x%08x.\n", status));
            __leave;
        }

        if (!ecpList) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "File name: %wZ\n", &fileNameInfo->Name));
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "No ECP list present during network create completion.\n"));
            __leave;
        }

        ULONG ecpCtxSize = 0;
        status = FltFindExtraCreateParameter(FltObjects->Filter, ecpList, &GUID_ECP_NETWORK_OPEN_CONTEXT, reinterpret_cast<PVOID*>(&netOpenEcpCtx), &ecpCtxSize);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "File name: %wZ\n", &fileNameInfo->Name));
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "No NETWORK_OPEN_ECP_CONTEXT in ECP list during network create completion.\n"));
            __leave;
        }

        if (netOpenEcpCtx->out.Location == NetworkOpenLocationLoopback) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "Network open with loopback location.\n"));
        } else {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "Network open with remote location.\n"));
        }
    } __finally {
        if (ctx) {
            FltReleaseContext(ctx);
        }
        if (fileNameInfo) {
            FltReleaseFileNameInformation(fileNameInfo);
        }
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

