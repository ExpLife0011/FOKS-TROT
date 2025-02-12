
#include "write.h"
#include "context.h"
#include "utils.h"
#include "cipher.h"
#include "filefuncs.h"


FLT_PREOP_CALLBACK_STATUS
PocPreWriteOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    NTSTATUS Status;

    CHAR ProcessName[POC_MAX_NAME_LENGTH] = { 0 };

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    BOOLEAN ContextCreated = FALSE;

    BOOLEAN NonCachedIo = FALSE;
    BOOLEAN PagingIo = FALSE;

    PCHAR OrigBuffer = NULL, NewBuffer = NULL;
    PMDL NewMdl = NULL;
    ULONG NewBufferLength = 0;

    PFSRTL_ADVANCED_FCB_HEADER AdvancedFcbHeader = NULL;
    ULONG FileSize = 0, StartingVbo = 0, ByteCount = 0, LengthReturned = 0;

    PPOC_VOLUME_CONTEXT VolumeContext = NULL;
    ULONG SectorSize = 0;

    PPOC_SWAP_BUFFER_CONTEXT SwapBufferContext = NULL;
    
    ByteCount = Data->Iopb->Parameters.Write.Length;
    StartingVbo = Data->Iopb->Parameters.Write.ByteOffset.LowPart;

    AdvancedFcbHeader = FltObjects->FileObject->FsContext;
    FileSize = AdvancedFcbHeader->FileSize.LowPart;

    NonCachedIo = BooleanFlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE);
    PagingIo = BooleanFlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO);


    if (0 == ByteCount)
    {
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto ERROR;
    }

    Status = PocGetProcessName(Data, ProcessName);


    Status = PocFindOrCreateStreamContext(
        Data->Iopb->TargetInstance,
        Data->Iopb->TargetFileObject,
        FALSE,
        &StreamContext,
        &ContextCreated);

    if (STATUS_SUCCESS != Status)
    {
        if (STATUS_NOT_FOUND != Status)
            DbgPrint("PocPreWriteOperation->PocFindOrCreateStreamContext failed. Status = 0x%x ProcessName = %s\n", 
                Status, ProcessName);
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto ERROR;
    }


    //DbgPrint("\nPocPreWriteOperation->enter StartingVbo = %d Length = %d ProcessName = %s File = %ws.\n NonCachedIo = %d PagingIo = %d\n",
    //    Data->Iopb->Parameters.Write.ByteOffset.LowPart,
    //    Data->Iopb->Parameters.Write.Length,
    //    ProcessName, StreamContext->FileName,
    //    NonCachedIo,
    //    PagingIo);

    if (POC_RENAME_TO_ENCRYPT == StreamContext->Flag)
    {
        DbgPrint("PocPreWriteOperation->leave PostClose will encrypt the file. StartingVbo = %d ProcessName = %s File = %ws.\n",
            Data->Iopb->Parameters.Write.ByteOffset.LowPart, ProcessName, StreamContext->FileName);
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto ERROR;
    }


    if (FltObjects->FileObject->SectionObjectPointer == StreamContext->ShadowSectionObjectPointers
        && NonCachedIo)
    {
        DbgPrint("PocPreWriteOperation->Block StartingVbo = %d ProcessName = %s File = %ws.\n",
            Data->Iopb->Parameters.Write.ByteOffset.LowPart, ProcessName, StreamContext->FileName);

        Data->IoStatus.Status = STATUS_SUCCESS;
        Data->IoStatus.Information = Data->Iopb->Parameters.Write.Length;

        Status = FLT_PREOP_COMPLETE;
        goto ERROR;
    }


    SwapBufferContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(POC_SWAP_BUFFER_CONTEXT), WRITE_BUFFER_TAG);

    if (NULL == SwapBufferContext)
    {
        DbgPrint("PocPreWriteOperation->ExAllocatePoolWithTag SwapBufferContext failed.\n");
        Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Data->IoStatus.Information = 0;
        Status = FLT_PREOP_COMPLETE;
        goto ERROR;
    }

    RtlZeroMemory(SwapBufferContext, sizeof(POC_SWAP_BUFFER_CONTEXT));


    if (NonCachedIo)
    {

        Status = FltGetVolumeContext(FltObjects->Filter, FltObjects->Volume, &VolumeContext);

        if (!NT_SUCCESS(Status) || 0 == VolumeContext->SectorSize)
        {
            DbgPrint("PocPostReadOperation->FltGetVolumeContext failed. Status = 0x%x\n", Status);
            goto EXIT;
        }

        SectorSize = VolumeContext->SectorSize;

        if (NULL != VolumeContext)
        {
            FltReleaseContext(VolumeContext);
            VolumeContext = NULL;
        }

        //LengthReturned是本次Write真正需要写的数据
        if (!PagingIo || FileSize >= StartingVbo + ByteCount)
        {
            LengthReturned = ByteCount;
        }
        else
        {
            LengthReturned = FileSize - StartingVbo;
        }

        DbgPrint("PocPreWriteOperation->RealToWrite = %d.\n", LengthReturned);
        
        if (Data->Iopb->Parameters.Write.MdlAddress != NULL) 
        {

            FLT_ASSERT(((PMDL)Data->Iopb->Parameters.Write.MdlAddress)->Next == NULL);

            OrigBuffer = MmGetSystemAddressForMdlSafe(Data->Iopb->Parameters.Write.MdlAddress,
                NormalPagePriority | MdlMappingNoExecute);

            if (OrigBuffer == NULL) 
            {
                DbgPrint("PocPreWriteOperation->Failed to get system address for MDL: %p\n",
                    Data->Iopb->Parameters.Write.MdlAddress);

                Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Data->IoStatus.Information = 0;
                Status = FLT_PREOP_COMPLETE;
                goto ERROR;
            }

        }
        else 
        {
            OrigBuffer = Data->Iopb->Parameters.Write.WriteBuffer;
        }

        

        if (FileSize > AES_BLOCK_SIZE &&
            LengthReturned < AES_BLOCK_SIZE)
        {
            NewBufferLength = SectorSize + ByteCount;
        }
        else
        {
            NewBufferLength = ByteCount;
        }

        NewBuffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, NonPagedPool, NewBufferLength, WRITE_BUFFER_TAG);

        if (NULL == NewBuffer)
        {
            DbgPrint("PocPreWriteOperation->FltAllocatePoolAlignedWithTag NewBuffer failed.\n");
            Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Data->IoStatus.Information = 0;
            Status = FLT_PREOP_COMPLETE;
            goto ERROR;
        }

        RtlZeroMemory(NewBuffer, NewBufferLength);

        if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_IRP_OPERATION)) 
        {

            NewMdl = IoAllocateMdl(NewBuffer, NewBufferLength, FALSE, FALSE, NULL);

            if (NewMdl == NULL) 
            {
                DbgPrint("PocPreWriteOperation->IoAllocateMdl NewMdl failed.\n");
                Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Data->IoStatus.Information = 0;
                Status = FLT_PREOP_COMPLETE;
                goto ERROR;
            }

            MmBuildMdlForNonPagedPool(NewMdl);
        }
        


        try 
        {

            if (FileSize < AES_BLOCK_SIZE)
            {
                /*
                * 文件小于一个块，采用流式加密
                */
                PocStreamModeEncrypt(OrigBuffer, LengthReturned, NewBuffer);

            }
            else if ((FileSize > StartingVbo + ByteCount) && 
                    (FileSize - (StartingVbo + ByteCount) < AES_BLOCK_SIZE))
            {
                /*
                * 当文件大于一个块，Cache Manager将数据分多次写入磁盘，
                * 最后一次写的数据小于一个块的情况下，现在在倒数第二个块做一下处理
                */

                if (SectorSize == ByteCount)
                {
                    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

                    RtlMoveMemory(StreamContext->PageNextToLastForWrite.Buffer, OrigBuffer, SectorSize);
                    StreamContext->PageNextToLastForWrite.StartingVbo = StartingVbo;
                    StreamContext->PageNextToLastForWrite.ByteCount = ByteCount;

                    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

                    Data->IoStatus.Status = STATUS_SUCCESS;
                    Data->IoStatus.Information = Data->Iopb->Parameters.Write.Length;

                    Status = FLT_PREOP_COMPLETE;
                    goto ERROR;
                }
                else if(ByteCount > SectorSize)
                {

                    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

                    RtlMoveMemory(StreamContext->PageNextToLastForWrite.Buffer, OrigBuffer + ByteCount - SectorSize, SectorSize);
                    StreamContext->PageNextToLastForWrite.StartingVbo = StartingVbo + ByteCount - SectorSize;
                    StreamContext->PageNextToLastForWrite.ByteCount = SectorSize;

                    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

                    LengthReturned = ByteCount - SectorSize;

                    Status = PocAesECBEncrypt(
                        OrigBuffer, 
                        LengthReturned, 
                        NewBuffer, 
                        &LengthReturned);

                    if (STATUS_SUCCESS != Status)
                    {
                        DbgPrint("PocPreWriteOperation->PocAesECBEncrypt1 failed.\n");
                        Data->IoStatus.Status = STATUS_UNSUCCESSFUL;
                        Data->IoStatus.Information = 0;
                        Status = FLT_PREOP_COMPLETE;
                        goto ERROR;
                    }

                    Data->Iopb->Parameters.Write.Length -= SectorSize;
                    FltSetCallbackDataDirty(Data);
                    SwapBufferContext->OriginalLength = ByteCount;
                }

            }
            else if (FileSize > AES_BLOCK_SIZE && 
                    LengthReturned < AES_BLOCK_SIZE)
            {
                /*
                * 当文件大于一个块，Cache Manager将数据分多次写入磁盘，最后一次写的数据小于一个块时
                */
                ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

                RtlMoveMemory(
                    StreamContext->PageNextToLastForWrite.Buffer + 
                    StreamContext->PageNextToLastForWrite.ByteCount, 
                    OrigBuffer, LengthReturned);

                ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

                LengthReturned = StreamContext->PageNextToLastForWrite.ByteCount + LengthReturned;

                Status = PocAesECBEncrypt_CiphertextStealing(
                    StreamContext->PageNextToLastForWrite.Buffer, 
                    LengthReturned,
                    NewBuffer);

                if (STATUS_SUCCESS != Status)
                {
                    DbgPrint("PocPreWriteOperation->PocAesECBEncrypt_CiphertextStealing1 failed.\n");
                    Data->IoStatus.Status = STATUS_UNSUCCESSFUL;
                    Data->IoStatus.Information = 0;
                    Status = FLT_PREOP_COMPLETE;
                    goto ERROR;
                }

                Data->Iopb->Parameters.Write.ByteOffset.LowPart = StreamContext->PageNextToLastForWrite.StartingVbo;
                Data->Iopb->Parameters.Write.Length = SectorSize + ByteCount;
                FltSetCallbackDataDirty(Data);

                SwapBufferContext->OriginalLength = ByteCount;

            }
            else if (LengthReturned % AES_BLOCK_SIZE != 0)
            {
                /*
                * 当需要写的数据大于一个块时，且和块大小不对齐时，这里用密文挪用的方式，不需要增加文件大小
                */

                Status = PocAesECBEncrypt_CiphertextStealing(
                    OrigBuffer, 
                    LengthReturned, 
                    NewBuffer);

                if (STATUS_SUCCESS != Status)
                {
                    DbgPrint("PocPreWriteOperation->PocAesECBEncrypt_CiphertextStealing2 failed.\n");
                    Data->IoStatus.Status = STATUS_UNSUCCESSFUL;
                    Data->IoStatus.Information = 0;
                    Status = FLT_PREOP_COMPLETE;
                    goto ERROR;
                }

            }
            else
            {
                /*
                * 当需要写的数据本身就和块大小对齐时，直接加密
                */

                Status = PocAesECBEncrypt(
                    OrigBuffer, 
                    LengthReturned, 
                    NewBuffer, 
                    &LengthReturned);

                if (STATUS_SUCCESS != Status)
                {
                    DbgPrint("PocPreWriteOperation->PocAesECBEncrypt2 failed.\n");
                    Data->IoStatus.Status = STATUS_UNSUCCESSFUL;
                    Data->IoStatus.Information = 0;
                    Status = FLT_PREOP_COMPLETE;
                    goto ERROR;
                }

            }

        }
        except(EXCEPTION_EXECUTE_HANDLER)
        {
            Data->IoStatus.Status = GetExceptionCode();
            Data->IoStatus.Information = 0;
            Status = FLT_PREOP_COMPLETE;
            goto ERROR;
        }



        SwapBufferContext->NewBuffer = NewBuffer;
        SwapBufferContext->NewMdl = NewMdl;
        SwapBufferContext->StreamContext = StreamContext;
        *CompletionContext = SwapBufferContext;

        Data->Iopb->Parameters.Write.WriteBuffer = NewBuffer;
        Data->Iopb->Parameters.Write.MdlAddress = NewMdl;
        FltSetCallbackDataDirty(Data);


        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        StreamContext->IsCipherText = TRUE;
        StreamContext->Flag = POC_TO_APPEND_ENCRYPTION_TAILER;

        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);


        DbgPrint("PocPreWriteOperation->Encrypt success. StartingVbo = %d Length = %d ProcessName = %s File = %ws.\n\n",
            Data->Iopb->Parameters.Write.ByteOffset.LowPart,
            LengthReturned,
            ProcessName,
            StreamContext->FileName);

        

        Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;
        goto EXIT;
    }


    *CompletionContext = SwapBufferContext;
    SwapBufferContext->StreamContext = StreamContext;
    Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;
    goto EXIT;

ERROR:

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    if (NULL != NewBuffer)
    {
        FltFreePoolAlignedWithTag(FltObjects->Instance, NewBuffer, WRITE_BUFFER_TAG);
        NewBuffer = NULL;
    }

    if (NULL != NewMdl)
    {
        IoFreeMdl(NewMdl);
        NewMdl = NULL;
    }

    if (NULL != SwapBufferContext)
    {
        ExFreePoolWithTag(SwapBufferContext, WRITE_BUFFER_TAG);
        SwapBufferContext = NULL;
    }

EXIT:

    return Status;
}


FLT_POSTOP_CALLBACK_STATUS
PocPostWriteOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);


    ASSERT(CompletionContext != NULL);
    ASSERT(((PPOC_SWAP_BUFFER_CONTEXT)CompletionContext)->StreamContext != NULL);


    PPOC_SWAP_BUFFER_CONTEXT SwapBufferContext = NULL;
    PPOC_STREAM_CONTEXT StreamContext = NULL;

    SwapBufferContext = CompletionContext;
    StreamContext = SwapBufferContext->StreamContext;


    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

    StreamContext->FileSize = ((PFSRTL_ADVANCED_FCB_HEADER)FltObjects->FileObject->FsContext)->FileSize.LowPart;

    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

    if (0 != SwapBufferContext->OriginalLength)
    {
        Data->IoStatus.Information = SwapBufferContext->OriginalLength;
    }


    if (NULL != SwapBufferContext->NewBuffer)
    {
        FltFreePoolAlignedWithTag(FltObjects->Instance, SwapBufferContext->NewBuffer, WRITE_BUFFER_TAG);
        SwapBufferContext->NewBuffer = NULL;
    }

    if (NULL != SwapBufferContext)
    {
        ExFreePoolWithTag(SwapBufferContext, WRITE_BUFFER_TAG);
        SwapBufferContext = NULL;
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }


    return FLT_POSTOP_FINISHED_PROCESSING;
}
