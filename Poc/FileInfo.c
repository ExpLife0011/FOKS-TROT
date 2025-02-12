
#include "fileinfo.h"
#include "context.h"
#include "utils.h"
#include "filefuncs.h"


FLT_PREOP_CALLBACK_STATUS
PocPreQueryInformationOperation(
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

    Status = PocGetProcessName(Data, ProcessName);


    Status = PocFindOrCreateStreamContext(
        Data->Iopb->TargetInstance,
        Data->Iopb->TargetFileObject,
        FALSE,
        &StreamContext,
        &ContextCreated);

    if (STATUS_SUCCESS != Status)
    {
        if (STATUS_NOT_FOUND != Status)      //说明不是目标扩展文件，在Create中没有创建StreamContext，不认为是个错误
            DbgPrint("PocPreQueryInformationOperation->PocFindOrCreateStreamContext failed. Status = 0x%x\n", Status);
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto EXIT;
    }

    if (!StreamContext->IsCipherText)
    {
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto EXIT;
    }

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION))
    {
        Status = FLT_PREOP_DISALLOW_FASTIO;
        goto EXIT;
    }


    /*DbgPrint("\nPocPreQueryInformationOperation->enter FileInformationClass = %d ProcessName = %s File = %ws.\n",
        Data->Iopb->Parameters.QueryFileInformation.FileInformationClass,
        ProcessName, StreamContext->FileName);*/

    *CompletionContext = StreamContext;
    Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;
    return Status;

EXIT:

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return Status;
}


FLT_POSTOP_CALLBACK_STATUS
PocPostQueryInformationOperation(
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

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    PVOID InfoBuffer = NULL;

    StreamContext = CompletionContext;
    InfoBuffer = Data->Iopb->Parameters.QueryFileInformation.InfoBuffer;

    switch (Data->Iopb->Parameters.QueryFileInformation.FileInformationClass) {

    case FileStandardInformation:
    {
        PFILE_STANDARD_INFORMATION Info = (PFILE_STANDARD_INFORMATION)InfoBuffer;
        Info->EndOfFile.LowPart = StreamContext->FileSize;
        break;
    }
    case FileAllInformation:
    {
        PFILE_ALL_INFORMATION Info = (PFILE_ALL_INFORMATION)InfoBuffer;
        if (Data->IoStatus.Information >=
            sizeof(FILE_BASIC_INFORMATION) +
            sizeof(FILE_STANDARD_INFORMATION))
        {
            Info->StandardInformation.EndOfFile.LowPart = StreamContext->FileSize;
        }
        break;
    }
    case FileEndOfFileInformation:
    {
        PFILE_END_OF_FILE_INFORMATION Info = (PFILE_END_OF_FILE_INFORMATION)InfoBuffer;
        Info->EndOfFile.LowPart = StreamContext->FileSize;
        break;
    }
    case FileNetworkOpenInformation:
    {
        PFILE_NETWORK_OPEN_INFORMATION Info = (PFILE_NETWORK_OPEN_INFORMATION)InfoBuffer;
        Info->EndOfFile.LowPart = StreamContext->FileSize;
        break;
    }
    default:
    {
        break;
    }
    }


    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}


FLT_PREOP_CALLBACK_STATUS
PocPreSetInformationOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    NTSTATUS Status;

    
    Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;

    return Status;
}


FLT_POSTOP_CALLBACK_STATUS
PocPostSetInformationOperation(
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

    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PFILE_OBJECT TargetFileObject = NULL;
    PFILE_RENAME_INFORMATION Buffer = NULL;

    WCHAR NewFileName[POC_MAX_NAME_LENGTH] = { 0 };
    WCHAR NewFileExtension[POC_MAX_NAME_LENGTH] = { 0 };

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    BOOLEAN ContextCreated = FALSE;

    UNICODE_STRING uFileName = { 0 };
    OBJECT_ATTRIBUTES ObjectAttributes = { 0 };
    HANDLE FileHandle = NULL;
    IO_STATUS_BLOCK IoStatusBlock = { 0 };

    CHAR ProcessName[POC_MAX_NAME_LENGTH] = { 0 };


    if (STATUS_SUCCESS != Data->IoStatus.Status)
    {
        Status = FLT_POSTOP_FINISHED_PROCESSING;
        goto EXIT;
    }


    switch (Data->Iopb->Parameters.SetFileInformation.FileInformationClass)
    {
    case FileRenameInformation:
    case FileRenameInformationEx:
    {

        TargetFileObject = Data->Iopb->Parameters.SetFileInformation.ParentOfTarget;

        if (NULL == TargetFileObject)
        {
            Buffer = Data->Iopb->Parameters.SetFileInformation.InfoBuffer;

            if (Buffer->FileNameLength < sizeof(NewFileName))
            {
                RtlMoveMemory(NewFileName, Buffer->FileName, Buffer->FileNameLength);
            }
        }
        else
        {
            if (wcslen(TargetFileObject->FileName.Buffer) * sizeof(WCHAR) < sizeof(NewFileName))
            {
                RtlMoveMemory(NewFileName, TargetFileObject->FileName.Buffer, wcslen(TargetFileObject->FileName.Buffer) * sizeof(WCHAR));
            }
        }

        Status = PocGetProcessName(Data, ProcessName);

        PocParseFileNameExtension(NewFileName, NewFileExtension);

        Status = PocFindOrCreateStreamContext(
            Data->Iopb->TargetInstance,
            Data->Iopb->TargetFileObject,
            FALSE,
            &StreamContext,
            &ContextCreated);


        if (STATUS_SUCCESS == Status)
        {
            /*
            * 到这里，说明是目标扩展名改成目标或非目标扩展名，这里即便已经是密文，我们也不修改尾部里的FileName
            * 当再一次Create时，会由PostCreate->PocCreateFileForEncTailer判断文件名是否一致，
            * 不一致则会交给PostClose修改Tailer
            */

            Status = PocBypassIrrelevantFileExtension(NewFileExtension);

            if (POC_IRRELEVENT_FILE_EXTENSION == Status || NULL != TargetFileObject)
            {
                if (NULL != StreamContext)
                {
                    FltReleaseContext(StreamContext);
                    StreamContext = NULL;
                }

                Status = FltDeleteStreamContext(Data->Iopb->TargetInstance, Data->Iopb->TargetFileObject, NULL);

                if (STATUS_SUCCESS == Status)
                {
                    DbgPrint("PocPostSetInformationOperation->FltDeleteStreamContext NewFileName = %ws.\n", NewFileName);
                }
                else
                {
                    DbgPrint("PocPostSetInformationOperation->FltDeleteStreamContext NewFileName = %ws failed. Status = 0x%x.\n", 
                        NewFileName, Status);
                }


            }
            else
            {
                DbgPrint("PocPostSetInformationOperation->PocUpdateNameInStreamContext %ws to %ws.\n",
                    StreamContext->FileName, NewFileName);

                Status = PocUpdateNameInStreamContext(StreamContext, NewFileName);

                if (STATUS_SUCCESS != Status)
                {
                    DbgPrint("PocPostSetInformationOperation->PocUpdateNameInStreamContext failed. Status = 0x%x\n", Status);
                    goto EXIT;
                }

            }

        }
        else if (STATUS_NOT_FOUND == Status)
        {
            /*
            * 到这里，说明原来的扩展名不是目标扩展名，所以没有进入PostCreate为其创建StreamContext
            */


            Status = PocBypassIrrelevantFileExtension(NewFileExtension);

            if (POC_IS_TARGET_FILE_EXTENSION == Status)
            {
                /*
                * POC_IS_TARGET_FILE_EXTENSION说明文件改成目标扩展名
                * 重入一波，让我们的PostCreate读一下是否有Tailer(有可能是目标扩展名密文->其他扩展名->目标扩展名的情况)，
                * 并且为其建立StreamContext
                */
                RtlZeroMemory(NewFileName, sizeof(NewFileName));

                PocGetFileNameOrExtension(Data, NULL, NewFileName);

                RtlInitUnicodeString(&uFileName, NewFileName);

                InitializeObjectAttributes(&ObjectAttributes, &uFileName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
                
                Status = ZwCreateFile(
                    &FileHandle, 
                    0, 
                    &ObjectAttributes, 
                    &IoStatusBlock, 
                    NULL, 
                    FILE_ATTRIBUTE_NORMAL, 
                    FILE_SHARE_READ | FILE_SHARE_WRITE, 
                    FILE_OPEN, 
                    FILE_NON_DIRECTORY_FILE, 
                    NULL, 
                    0);

                if (STATUS_SUCCESS != Status)
                {
                    DbgPrint("PocPostSetInformationOperation->ZwCreateFile failed. Status = 0x%x\n", Status);
                    goto EXIT;
                }

                Status = PocFindOrCreateStreamContext(
                    Data->Iopb->TargetInstance,
                    Data->Iopb->TargetFileObject,
                    FALSE,
                    &StreamContext,
                    &ContextCreated);

                if (STATUS_SUCCESS != Status)
                {
                    DbgPrint("PocPostSetInformationOperation->PocFindOrCreateStreamContext failed. Status = 0x%x\n", Status);
                }

                if (NULL != StreamContext)
                {
                    if (POC_TAILER_WRONG_FILE_NAME != StreamContext->Flag &&
                        POC_FILE_HAS_ENCRYPTION_TAILER != StreamContext->Flag)
                    {
                        PocUpdateFlagInStreamContext(StreamContext, POC_RENAME_TO_ENCRYPT);
                    }

                    DbgPrint("PocPostSetInformationOperation->other extension rename to target extension NewFileName = %ws Flag = 0x%x.\n\n",
                        NewFileName, StreamContext->Flag);
                }
                else
                {
                    DbgPrint("PocPostSetInformationOperation->other extension rename to target extension NewFileName = %ws.\n\n",
                        NewFileName);
                }
                
            }
        }
        else
        {
            DbgPrint("PocPostSetInformationOperation->PocFindOrCreateStreamContext failed. Status = 0x%x ProcessName = %s NewFileName = %ws\n",
                Status, ProcessName, NewFileName);
        }

        break;
    }
    }

EXIT:

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    if (NULL != FileHandle)
    {
        FltClose(FileHandle);
        FileHandle = NULL;
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}
