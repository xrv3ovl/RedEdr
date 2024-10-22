#include <stdio.h>

#include "pch.h"
#include "minhook/include/MinHook.h"
#include "../Shared/common.h"
#include <winternl.h>  // needs to be on bottom?
#include <dbghelp.h>

#include "dllhelper.h"
#include "logging.h"
#include "detours.h"

BOOL skip_self_readprocess = TRUE;
BOOL skip_rw_r_virtualprotect = FALSE; // TODO

// The hooking library will itself execute Nt* functions
// which it already hooked. Filter out all events from the hooking thread
DWORD MyThreadId = -1;


wchar_t* GetMemoryPermissions(wchar_t* buf, DWORD protection) {
    //char permissions[4] = "---"; // Initialize as "---"
    wcscpy_s(buf, 16, L"---");

    if (protection & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
        buf[0] = L'R'; // Readable
    }
    if (protection & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
        buf[1] = L'W'; // Writable
    }
    if (protection & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
        buf[2] = L'X'; // Executable
    }
    buf[3] = L'\x00';

    return buf;
}

/******************* AllocateVirtualMemory ************************/


typedef NTSTATUS(NTAPI* t_NtAllocateVirtualMemory)(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect
    );
t_NtAllocateVirtualMemory Real_NtAllocateVirtualMemory = NULL;

static NTSTATUS NTAPI Catch_NtAllocateVirtualMemory(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect)
{
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    if ((DWORD)GetCurrentThreadId() != MyThreadId) { // dont log our own hooking
        wchar_t protect_str[16] = L"";
        memset(protect_str, 0, sizeof(protect_str));
        GetMemoryPermissions(protect_str, Protect);

        int offset = 0;
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:AllocateVirtualMemory;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%p;", ProcessHandle);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"base_addr:%p;", BaseAddress);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"zero:%#llx;", ZeroBits);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"size:%llu;", *RegionSize);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:%#lx;", AllocationType);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"protect:%#lx;", Protect);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"protect_str:%ls;", protect_str);

        // BROKEN for some reason. Do not attempt to enable it again.
        //LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
        SendDllPipe(buf);
    }
    return Real_NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect);
}


/******************* ProtectVirtualMemory ************************/



typedef NTSTATUS(NTAPI* t_NtProtectVirtualMemory)(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    PULONG NumberOfBytesToProtect,
    ULONG NewAccessProtection,
    PULONG OldAccessProtection
    );
t_NtProtectVirtualMemory Real_NtProtectVirtualMemory = NULL;

static NTSTATUS NTAPI Catch_NtProtectVirtualMemory(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    PULONG NumberOfBytesToProtect,
    ULONG NewAccessProtection,
    PULONG OldAccessProtection
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    
    if ((DWORD)GetCurrentThreadId() != MyThreadId) { // dont log our own hooking
        wchar_t mem_perm[16] = L"";
        memset(mem_perm, 0, sizeof(mem_perm));
        GetMemoryPermissions(mem_perm, NewAccessProtection);

        int offset = 0;
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:ProtectVirtualMemory;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%p;", ProcessHandle);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"base_addr:%p;", BaseAddress);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"size:%lu;", *NumberOfBytesToProtect);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"protect:%#lx;", NewAccessProtection);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"protect_str:%s;", mem_perm);

        LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
        SendDllPipe(buf);
    }
    return Real_NtProtectVirtualMemory(ProcessHandle, BaseAddress, NumberOfBytesToProtect, NewAccessProtection, OldAccessProtection);
}


/******************* MapViewOfSection ************************/

typedef enum _SECTION_INHERIT {
    ViewShare = 1,   // Share the section.
    ViewUnmap = 2    // Unmap the section when the process terminates.
} SECTION_INHERIT;

// Defines the prototype of the NtMapViewOfSectionFunction
typedef NTSTATUS(NTAPI* t_NtMapViewOfSection)(
    HANDLE          SectionHandle,
    HANDLE          ProcessHandle,
    PVOID*          BaseAddress,
    ULONG_PTR       ZeroBits,
    SIZE_T          CommitSize,
    PLARGE_INTEGER  SectionOffset,
    PSIZE_T         ViewSize,
    DWORD           InheritDisposition,
    ULONG           AllocationType,
    ULONG           Protect
    );
t_NtMapViewOfSection Real_NtMapViewOfSection = NULL;
NTSTATUS NTAPI Catch_NtMapViewOfSection(
    HANDLE          SectionHandle,
    HANDLE          ProcessHandle,
    PVOID*          BaseAddress,
    ULONG_PTR       ZeroBits,
    SIZE_T          CommitSize,
    PLARGE_INTEGER  SectionOffset,
    PSIZE_T         ViewSize,
    SECTION_INHERIT InheritDisposition,
    ULONG           AllocationType,
    ULONG           Protect
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    wchar_t mem_perm[16] = L"";
    memset(mem_perm, 0, sizeof(mem_perm));

    GetMemoryPermissions(mem_perm, Protect);

    // Check if pointers are not NULL before dereferencing
    LONGLONG sectionOffsetValue = (SectionOffset != NULL) ? SectionOffset->QuadPart : 0;
    SIZE_T viewSizeValue = (ViewSize != NULL) ? *ViewSize : 0;
    PVOID baseAddressValue = (BaseAddress != NULL) ? *BaseAddress : NULL;
    
    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:MapViewOfSection;section_handle:0x%p;process_handle:0x%p;base_address:0x%p;zero_bits:%llu;size:%llu;section_offset:%lld;view_size:%llu;inherit_disposition:%x;alloc_type:%x;protect:%x;protect_str:%ls",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(),
    //    SectionHandle, ProcessHandle, baseAddressValue, ZeroBits, CommitSize,
    //    sectionOffsetValue, viewSizeValue, InheritDisposition, AllocationType, Protect, mem_perm);

    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:MapViewOfSection;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"section_handle:0x%p;", SectionHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"process_handle:0x%p;", ProcessHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"base_address:0x%p;", baseAddressValue);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"zero_bits:%llu;", ZeroBits);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"size:%llu;", CommitSize);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"section_offset:%lld;", sectionOffsetValue);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"view_size:%llu;", viewSizeValue);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"inherit_disposition:%x;", InheritDisposition);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"alloc_type:%x;", AllocationType);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"protect:%x;", Protect);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"protect_str:%s;", mem_perm);
    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType, Protect);
}


/******************* WriteVirtualMemory ************************/

// Defines the prototype of the NtWriteVirtualMemoryFunction
typedef NTSTATUS(NTAPI* t_NtWriteVirtualMemory)(
    HANDLE              ProcessHandle,
    PVOID               BaseAddress,
    PVOID               Buffer,
    ULONG               NumberOfBytesToWrite,
    PULONG              NumberOfBytesWritten
);
t_NtWriteVirtualMemory Real_NtWriteVirtualMemory = NULL;
NTSTATUS NTAPI Catch_NtWriteVirtualMemory(
    HANDLE              ProcessHandle,
    PVOID               BaseAddress,
    PVOID               Buffer,
    ULONG               NumberOfBytesToWrite,
    PULONG              NumberOfBytesWritten
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:WriteVirtualMemory;process_handle:0x%p;base_address:0x%p;buffer:0x%p;size:%lu",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(),
    //    ProcessHandle, BaseAddress, Buffer, NumberOfBytesToWrite);

    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:WriteVirtualMemory;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"process_handle:0x%p;", ProcessHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"base_address:0x%p;", BaseAddress);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"buffer:0x%p;", Buffer);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"size:%lu;", NumberOfBytesToWrite);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtWriteVirtualMemory(ProcessHandle, BaseAddress, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten);
}


/******************* ReadVirtualMemory ************************/

// Defines the prototype of the NtReadVirtualMemory function
typedef NTSTATUS(NTAPI* pNtReadVirtualMemory)(
    HANDLE              ProcessHandle,
    PVOID               BaseAddress,
    PVOID               Buffer,
    ULONG               NumberOfBytesToRead,
    PULONG              NumberOfBytesRead
    );
pNtReadVirtualMemory Real_NtReadVirtualMemory = NULL;
NTSTATUS NTAPI Catch_NtReadVirtualMemory(
    HANDLE              ProcessHandle,
    PVOID               BaseAddress,
    PVOID               Buffer,
    ULONG               NumberOfBytesToRead,
    PULONG              NumberOfBytesRead
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:ReadVirtualMemory;process_handle:0x%p;base_address:0x%p;buffer:0x%p;size:%lu",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(),
    //    ProcessHandle, BaseAddress, Buffer, NumberOfBytesToRead);

    if (!skip_self_readprocess || ProcessHandle != (HANDLE)-1) {
        int offset = 0;
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:ReadVirtualMemory;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"process_handle:0x%p;", ProcessHandle);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"base_address:0x%p;", BaseAddress);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"buffer:0x%p;", Buffer);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"size:%lu;", NumberOfBytesToRead);
        SendDllPipe(buf);
    }

    // Currently makes notepad.exe crash on save dialog open on win11.
    // And its a lot of data
    //LogMyStackTrace(&buf[ret], DATA_BUFFER_SIZE - ret);

    return Real_NtReadVirtualMemory(ProcessHandle, BaseAddress, Buffer, NumberOfBytesToRead, NumberOfBytesRead);
}


/******************* NtSetContextThread ************************/

// Defines the prototype of the NtSetContextThreadFunction
typedef NTSTATUS(NTAPI* pNtSetContextThread)(
    IN HANDLE               ThreadHandle,
    IN PCONTEXT             Context
    );
pNtSetContextThread Real_NtSetContextThread = NULL;
NTSTATUS NTAPI Catch_NtSetContextThread(
    IN HANDLE               ThreadHandle,
    IN PCONTEXT             Context
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:SetContextThread;thread_handle:0x%p",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ThreadHandle);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:SetContextThread;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"thread_handle:0x%p;", ThreadHandle);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    
    return Real_NtSetContextThread(ThreadHandle, Context);
}


/******************* LdrLoadDll ************************/

typedef NTSTATUS(NTAPI* pLdrLoadDll)(
    IN PWSTR            SearchPath          OPTIONAL,
    IN PULONG           DllCharacteristics  OPTIONAL,
    IN PUNICODE_STRING  DllName,
    OUT PVOID*          BaseAddress
    );
pLdrLoadDll Real_LdrLoadDll = NULL;
NTSTATUS NTAPI Catch_LdrLoadDll(
    IN PWSTR            SearchPath          OPTIONAL,
    IN PULONG           DllCharacteristics  OPTIONAL,
    IN PUNICODE_STRING  DllName,
    OUT PVOID* BaseAddress
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    wchar_t wDllName[1024] = L"";  // Buffer for the decoded DllName
    wchar_t empty[32] = L"<broken>";        // Empty string in case SearchPath is NULL
    
    wchar_t* searchPath = empty;   // SearchPath seems to be 8 (the number 8, not a string) BROKEN
    UnicodeStringToWChar(DllName, wDllName, 1024);
    ULONG dllCharacteristics = (DllCharacteristics != NULL) ? *DllCharacteristics : 0;

   /* int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
        L"type:dll;time:%llu;krn_pid:%llu;func:LdrLoadDll;search_path:%ls;dll_characteristics:0x%lx;dll_name:%ls",
        time.QuadPart,
        (unsigned __int64)GetCurrentProcessId(),
        searchPath,
        dllCharacteristics,
        wDllName);*/

    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:LdrLoadDll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"search_path:%ls;", searchPath);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"dll_characteristics:0x%lx;", dllCharacteristics);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"dll_name:%ls;", wDllName);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_LdrLoadDll(SearchPath, DllCharacteristics, DllName, BaseAddress);
}


/******************* LdrGetProcedureAddress ************************/

typedef NTSTATUS(NTAPI* pLdrGetProcedureAddress)(
    IN HMODULE              ModuleHandle,
    IN PANSI_STRING         FunctionName,
    IN WORD                 Oridinal,
    OUT FARPROC* FunctionAddress
    );
pLdrGetProcedureAddress Real_LdrGetProcedureAddress = NULL;
NTSTATUS NTAPI Catch_LdrGetProcedureAddress(
    IN HMODULE              ModuleHandle,
    IN PANSI_STRING         FunctionName,
    IN WORD                 Oridinal,
    OUT FARPROC* FunctionAddress
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    wchar_t wideFunctionName[1024] = L"";
    //UnicodeStringToWChar(FunctionName, wideFunctionName, 1024);

    if (FunctionName && FunctionName->Buffer) {
        // Convert ANSI string to wide string
        MultiByteToWideChar(CP_ACP, 0, FunctionName->Buffer, -1, wideFunctionName, 1024);
    }
    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:LdrGetProcedureAddress;module_handle:0x%p;function:%s;ordinal:0x%hx",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ModuleHandle, wideFunctionName, Oridinal);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:LdrGetProcedureAddress;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"module_handle:0x%p;", ModuleHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"function:%s;", wideFunctionName);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"ordinal:0x%hx;", Oridinal);

    SendDllPipe(buf);

    return Real_LdrGetProcedureAddress(ModuleHandle, FunctionName, Oridinal, FunctionAddress);
}


/******************* NtQueueApcThread ************************/

typedef NTSTATUS(NTAPI* pNtQueueApcThread)(
    IN HANDLE               ThreadHandle, 
    IN PIO_APC_ROUTINE      ApcRoutine,
    IN PVOID                ApcRoutineContext OPTIONAL,
    IN PIO_STATUS_BLOCK     ApcStatusBlock OPTIONAL,
    IN ULONG                ApcReserved OPTIONAL
    );
pNtQueueApcThread Real_NtQueueApcThread = NULL;
NTSTATUS NTAPI Catch_NtQueueApcThread(
    IN HANDLE               ThreadHandle,    
    IN PIO_APC_ROUTINE      ApcRoutine,
    IN PVOID                ApcRoutineContext OPTIONAL,
    IN PIO_STATUS_BLOCK     ApcStatusBlock OPTIONAL,
    IN ULONG                ApcReserved OPTIONAL
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtQueueApcThread;thread_handle:0x%p",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ThreadHandle);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtQueueApcThread;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"thread_handle:0x%p;", ThreadHandle);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtQueueApcThread(ThreadHandle, ApcRoutine, ApcRoutineContext, ApcStatusBlock, ApcReserved);
}


/******************* NtQueueApcThreadEx ************************/

typedef NTSTATUS(NTAPI* pNtQueueApcThreadEx)(
    IN HANDLE               ThreadHandle,
    IN HANDLE               ApcThreadHandle,
    IN PVOID                ApcRoutine,
    IN PVOID                ApcArgument1,
    IN PVOID                ApcArgument2,
    IN PVOID                ApcArgument3
    );
pNtQueueApcThreadEx Real_NtQueueApcThreadEx = NULL;
NTSTATUS NTAPI Catch_NtQueueApcThreadEx(
    IN HANDLE               ThreadHandle,
    IN HANDLE               ApcThreadHandle,
    IN PVOID                ApcRoutine,
    IN PVOID                ApcArgument1,
    IN PVOID                ApcArgument2,
    IN PVOID                ApcArgument3
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    OutputDebugString(L"A8");

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtQueueApcThreadEx;thread_handle:0x%p;apc_thread:0x%p;apc_routine:0x%p;arg1:0x%p;arg2:0x%p;arg3:0x%p",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ThreadHandle, ApcThreadHandle, ApcRoutine, ApcArgument1, ApcArgument2, ApcArgument3);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtQueueApcThreadEx;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"thread_handle:0x%p;", ThreadHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"apc_thread:0x%p;", ApcThreadHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"apc_routine:0x%p;", ApcRoutine);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"arg1:0x%p;", ApcArgument1);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"arg2:0x%p;", ApcArgument2);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"arg3:0x%p;", ApcArgument3);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtQueueApcThreadEx(ThreadHandle, ApcThreadHandle, ApcRoutine, ApcArgument1, ApcArgument2, ApcArgument3);
}


/******************* NtCreateProcess ************************/

typedef NTSTATUS(NTAPI* pNtCreateProcess)(
    OUT PHANDLE             ProcessHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN HANDLE               ParentProcess,
    IN BOOLEAN              InheritObjectTable,
    IN HANDLE               SectionHandle,
    IN HANDLE               DebugPort,
    IN HANDLE               ExceptionPort
    );
pNtCreateProcess Real_NtCreateProcess = NULL;
NTSTATUS NTAPI Catch_NtCreateProcess(
    OUT PHANDLE             ProcessHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN HANDLE               ParentProcess,
    IN BOOLEAN              InheritObjectTable,
    IN HANDLE               SectionHandle,
    IN HANDLE               DebugPort,
    IN HANDLE               ExceptionPort
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    OutputDebugString(L"A9");

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateProcess;process_handle:0x%p;access_mask:0x%x;parent_process:0x%p;inherit_table:%d",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ProcessHandle, DesiredAccess, ParentProcess, InheritObjectTable);

    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateProcess;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"process_handle:0x%p;", ProcessHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"access_mask:0x%x;", DesiredAccess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"parent_process:0x%p;", ParentProcess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"inherit_table:%d;", InheritObjectTable);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtCreateProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ParentProcess, InheritObjectTable, SectionHandle, DebugPort, ExceptionPort);
}


/******************* NtCreateThreadEx ************************/

typedef NTSTATUS(NTAPI* pNtCreateThreadEx)(
    OUT PHANDLE             ThreadHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN HANDLE               ProcessHandle,
    IN PVOID                StartRoutine,
    IN PVOID                Argument,
    IN ULONG                CreateFlags,
    IN ULONG_PTR            ZeroBits,
    IN SIZE_T               StackSize,
    IN SIZE_T               MaximumStackSize,
    IN PVOID                AttributeList
    );
pNtCreateThreadEx Real_NtCreateThreadEx = NULL;
NTSTATUS NTAPI Catch_NtCreateThreadEx(
    OUT PHANDLE             ThreadHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN HANDLE               ProcessHandle,
    IN PVOID                StartRoutine,
    IN PVOID                Argument,
    IN ULONG                CreateFlags,
    IN ULONG_PTR            ZeroBits,
    IN SIZE_T               StackSize,
    IN SIZE_T               MaximumStackSize,
    IN PVOID                AttributeList
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateThreadEx;thread_handle:0x%p;process_handle:0x%p;start_routine:0x%p;argument:0x%p",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), 
    //    ThreadHandle, ProcessHandle, StartRoutine, Argument);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateThreadEx;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"thread_handle:0x%p;", ThreadHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"process_handle:0x%p;", ProcessHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"start_routine:0x%p;", StartRoutine);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"argument:0x%p;", Argument);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtCreateThreadEx(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize, AttributeList);
}


/******************* NtOpenProcess ************************/

typedef NTSTATUS(NTAPI* pNtOpenProcess)(
    OUT PHANDLE             ProcessHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN CLIENT_ID*           ClientId
    );
pNtOpenProcess Real_NtOpenProcess = NULL;
NTSTATUS NTAPI Catch_NtOpenProcess(
    OUT PHANDLE             ProcessHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN CLIENT_ID*           ClientId
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtOpenProcess;process_handle:0x%p;access_mask:0x%x;client_id_process:0x%p;client_id_thread:0x%p",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ProcessHandle, DesiredAccess, ClientId->UniqueProcess, ClientId->UniqueThread);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtOpenProcess;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"process_handle:0x%p;", ProcessHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"access_mask:0x%x;", DesiredAccess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"client_id_process:0x%p;", ClientId->UniqueProcess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"client_id_thread:0x%p;", ClientId->UniqueThread);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
}


/******************* NtLoadDriver ************************/

typedef NTSTATUS(NTAPI* pNtLoadDriver)(
    IN PUNICODE_STRING      DriverServiceName
    );
pNtLoadDriver Real_NtLoadDriver = NULL;
NTSTATUS NTAPI Catch_NtLoadDriver(
    IN PUNICODE_STRING      DriverServiceName
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    wchar_t wDriverServiceName[1024];

    OutputDebugString(L"A12");

    UnicodeStringToWChar(DriverServiceName, wDriverServiceName, 1024);

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtLoadDriver;driver_service_name:%ls",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), wDriverServiceName);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtLoadDriver;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"driver_service_name:%ls;", wDriverServiceName);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtLoadDriver(DriverServiceName);
}


/******************* NtCreateNamedPipeFile ************************/

typedef NTSTATUS(NTAPI* pNtCreateNamedPipeFile)(
    OUT PHANDLE             NamedPipeFileHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    OUT PIO_STATUS_BLOCK    IoStatusBlock,
    IN ULONG                ShareAccess,
    IN ULONG                CreateDisposition,
    IN ULONG                CreateOptions,
    IN ULONG                NamedPipeType,
    IN ULONG                ReadMode,
    IN ULONG                CompletionMode,
    IN ULONG                MaximumInstances,
    IN ULONG                InboundQuota,
    IN ULONG                OutboundQuota,
    IN PLARGE_INTEGER       DefaultTimeout
    );
pNtCreateNamedPipeFile Real_NtCreateNamedPipeFile = NULL;
NTSTATUS NTAPI Catch_NtCreateNamedPipeFile(
    OUT PHANDLE             NamedPipeFileHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    OUT PIO_STATUS_BLOCK    IoStatusBlock,
    IN ULONG                ShareAccess,
    IN ULONG                CreateDisposition,
    IN ULONG                CreateOptions,
    IN ULONG                NamedPipeType,
    IN ULONG                ReadMode,
    IN ULONG                CompletionMode,
    IN ULONG                MaximumInstances,
    IN ULONG                InboundQuota,
    IN ULONG                OutboundQuota,
    IN PLARGE_INTEGER       DefaultTimeout
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    OutputDebugString(L"A13");

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateNamedPipeFile;pipe_handle:0x%p;access_mask:0x%x;share_access:0x%x;pipe_type:0x%x;read_mode:0x%x",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), NamedPipeFileHandle, DesiredAccess, ShareAccess, NamedPipeType, ReadMode);
    
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateNamedPipeFile;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pipe_handle:0x%p;", NamedPipeFileHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"access_mask:0x%x;", DesiredAccess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"share_access:0x%x;", ShareAccess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pipe_type:0x%x;", NamedPipeType);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"read_mode:0x%x;", ReadMode);

    SendDllPipe(buf);
    return Real_NtCreateNamedPipeFile(NamedPipeFileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, CreateDisposition, CreateOptions, NamedPipeType, ReadMode, CompletionMode, MaximumInstances, InboundQuota, OutboundQuota, DefaultTimeout);
}


/******************* NtOpenThread ************************/

typedef NTSTATUS(NTAPI* pNtOpenThread)(
    OUT PHANDLE             ThreadHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN CLIENT_ID*           ClientId
    );
pNtOpenThread Real_NtOpenThread = NULL;
NTSTATUS NTAPI Catch_NtOpenThread(
    OUT PHANDLE             ThreadHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN CLIENT_ID*           ClientId
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtOpenThread;thread_handle:0x%p;access_mask:0x%x;client_id_process:0x%p;client_id_thread:0x%p",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ThreadHandle, DesiredAccess, ClientId->UniqueProcess, ClientId->UniqueThread);
    
    if ((DWORD)GetCurrentThreadId() != MyThreadId) { // dont log our own hooking
        int offset = 0;
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtOpenThread;");
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"thread_handle:0x%p;", ThreadHandle);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"access_mask:0x%x;", DesiredAccess);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"client_id_process:0x%p;", ClientId->UniqueProcess);
        offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"client_id_thread:0x%p;", ClientId->UniqueThread);

        LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
        SendDllPipe(buf);
    }
    return Real_NtOpenThread(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId);
}


/******************* NtCreateSection ************************/

typedef NTSTATUS(NTAPI* pNtCreateSection)(
    OUT PHANDLE             SectionHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN PLARGE_INTEGER       MaximumSize,
    IN ULONG                SectionPageProtection,
    IN ULONG                AllocationAttributes,
    IN HANDLE               FileHandle
    );
pNtCreateSection Real_NtCreateSection = NULL;
NTSTATUS NTAPI Catch_NtCreateSection(
    OUT PHANDLE             SectionHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN PLARGE_INTEGER       MaximumSize,
    IN ULONG                SectionPageProtection,
    IN ULONG                AllocationAttributes,
    IN HANDLE               FileHandle
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateSection;section_handle:0x%p;access_mask:0x%x;max_size:0x%p;page_protection:0x%x;alloc_attributes:0x%x;file_handle:0x%p",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), SectionHandle, DesiredAccess, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateSection;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"section_handle:0x%p;", SectionHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"access_mask:0x%x;", DesiredAccess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"max_size:0x%p;", MaximumSize);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"page_protection:0x%x;", SectionPageProtection);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"alloc_attributes:0x%x;", AllocationAttributes);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"file_handle:0x%p;", FileHandle);
    
    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle);
}


/******************* NtCreateProcessEx ************************/

typedef NTSTATUS(NTAPI* pNtCreateProcessEx)(
    OUT PHANDLE             ProcessHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN HANDLE               ParentProcess,
    IN ULONG                Flags,
    IN HANDLE               SectionHandle,
    IN HANDLE               DebugPort,
    IN HANDLE               ExceptionPort,
    IN BOOLEAN              InJob
    );
pNtCreateProcessEx Real_NtCreateProcessEx = NULL;
NTSTATUS NTAPI Catch_NtCreateProcessEx(
    OUT PHANDLE             ProcessHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes,
    IN HANDLE               ParentProcess,
    IN ULONG                Flags,
    IN HANDLE               SectionHandle,
    IN HANDLE               DebugPort,
    IN HANDLE               ExceptionPort,
    IN BOOLEAN              InJob
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    OutputDebugString(L"A16");

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateProcessEx;process_handle:0x%p;parent_process:0x%p;flags:0x%lx;section_handle:0x%p;debug_port:0x%p;exception_port:0x%p;in_job:%d",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), ProcessHandle, ParentProcess, Flags, SectionHandle, DebugPort, ExceptionPort, InJob);
    
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateProcessEx;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"process_handle:0x%p;", ProcessHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"parent_process:0x%p;", ParentProcess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"flags:0x%lx;", Flags);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"section_handle:0x%p;", SectionHandle);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"debug_port:0x%p;", DebugPort);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"exception_port:0x%p;", ExceptionPort);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"in_job:%d;", InJob);

    LogMyStackTrace(&buf[offset], DATA_BUFFER_SIZE - offset);
    SendDllPipe(buf);
    return Real_NtCreateProcessEx(ProcessHandle, DesiredAccess, ObjectAttributes, ParentProcess, Flags, SectionHandle, DebugPort, ExceptionPort, InJob);
}


/******************* NtCreateEvent ************************/

typedef enum _EVENT_TYPE {
    NotificationEvent = 0,
    SynchronizationEvent = 1
} EVENT_TYPE;

typedef NTSTATUS(NTAPI* pNtCreateEvent)(
    OUT PHANDLE             EventHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes OPTIONAL,
    IN EVENT_TYPE           EventType,
    IN BOOLEAN              InitialState
    );
pNtCreateEvent Real_NtCreateEvent = NULL;
NTSTATUS NTAPI Catch_NtCreateEvent(
    OUT PHANDLE             EventHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes OPTIONAL,
    IN EVENT_TYPE           EventType,
    IN BOOLEAN              InitialState
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateEvent;desired_access:0x%x;event_type:%d;initial_state:%d",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), DesiredAccess, EventType, InitialState);
    
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateEvent;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"desired_access:0x%x;", DesiredAccess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"event_type:%d;", EventType);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"initial_state:%d;", InitialState);

    SendDllPipe(buf);
    return Real_NtCreateEvent(EventHandle, DesiredAccess, ObjectAttributes, EventType, InitialState);
}


/******************* NtCreateTimer ************************/

typedef enum _TIMER_TYPE {
    NotificationTimer,
    SynchronizationTimer
} TIMER_TYPE;

typedef NTSTATUS(NTAPI* pNtCreateTimer)(
    OUT PHANDLE             TimerHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes OPTIONAL,
    IN TIMER_TYPE           TimerType
    );
pNtCreateTimer Real_NtCreateTimer = NULL;
NTSTATUS NTAPI Catch_NtCreateTimer(
    OUT PHANDLE             TimerHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes OPTIONAL,
    IN TIMER_TYPE           TimerType
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";
    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateTimer;desired_access:0x%x;timer_type:%d",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), DesiredAccess, TimerType);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateTimer;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"desired_access:0x%x;", DesiredAccess);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"timer_type:%d;", TimerType);

    
    SendDllPipe(buf);
    return Real_NtCreateTimer(TimerHandle, DesiredAccess, ObjectAttributes, TimerType);
}


/******************* NtCreateTimer2 ************************/

typedef NTSTATUS(NTAPI* pNtCreateTimer2)(
    OUT PHANDLE             TimerHandle,
    IN PVOID                Reserved1 OPTIONAL,
    IN PVOID                Reserved2 OPTIONAL,
    IN ULONG                Attributes,
    IN ACCESS_MASK          DesiredAccess
    );
pNtCreateTimer2 Real_NtCreateTimer2 = NULL;
NTSTATUS NTAPI Catch_NtCreateTimer2(
    OUT PHANDLE             TimerHandle,
    IN PVOID                Reserved1 OPTIONAL,
    IN PVOID                Reserved2 OPTIONAL,
    IN ULONG                Attributes,
    IN ACCESS_MASK          DesiredAccess
) {
    LARGE_INTEGER time = get_time();
    wchar_t buf[DATA_BUFFER_SIZE] = L"";

    //int ret = swprintf_s(buf, DATA_BUFFER_SIZE,
    //    L"type:dll;time:%llu;krn_pid:%llu;func:NtCreateTimer2;attributes:0x%lx;desired_access:0x%x",
    //    time.QuadPart, (unsigned __int64)GetCurrentProcessId(), Attributes, DesiredAccess);
    int offset = 0;
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"type:dll;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"time:%llu;", time.QuadPart);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"pid:%lu;", (DWORD)GetCurrentProcessId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"tid:%lu;", (DWORD)GetCurrentThreadId());
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"func:NtCreateTimer2;");
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"attributes:0x%lx;", Attributes);
    offset += swprintf_s(buf + offset, DATA_BUFFER_SIZE - offset, L"desired_access:0x%x;", DesiredAccess);
    
    SendDllPipe(buf);
    return Real_NtCreateTimer2(TimerHandle, Reserved1, Reserved2, Attributes, DesiredAccess);
}


//----------------------------------------------------


// This function initializes the hooks via the MinHook library
DWORD WINAPI InitHooksThread(LPVOID param) {
    LONG error;

    if (DetourIsHelperProcess()) {
        return TRUE;
    }
    MyThreadId = GetCurrentThreadId();
    wchar_t start_str[1024] = { 0 };
    wchar_t stop_str[1024] = { 0 };

    swprintf(start_str, 1024, L"type:dll;func:hooking_start;pid:%lu;tid:%lu;",
        (DWORD)GetCurrentProcessId(), (DWORD)GetCurrentThreadId());
    swprintf(stop_str, 1024, L"type:dll;func:hooking_finished;pid:%lu;tid:%lu;",
        (DWORD)GetCurrentProcessId(), (DWORD)GetCurrentThreadId());

    LOG_A(LOG_INFO, "Injected DLL Detours Main thread started on pid %lu  threadid %lu", 
        GetCurrentProcessId(), GetCurrentThreadId());
    InitDllPipe();
    SendDllPipe(start_str);

    // All the original methods
    Real_NtSetContextThread = (pNtSetContextThread)DetourFindFunction("ntdll.dll", "Catch_NtSetContextThread");
    Real_LdrLoadDll = (pLdrLoadDll)DetourFindFunction("ntdll.dll", "LdrLoadDll");
    Real_LdrGetProcedureAddress = (pLdrGetProcedureAddress)DetourFindFunction("ntdll.dll", "LdrGetProcedureAddress");
    Real_NtQueueApcThread = (pNtQueueApcThread)DetourFindFunction("ntdll.dll", "NtQueueApcThread");
    Real_NtQueueApcThreadEx = (pNtQueueApcThreadEx)DetourFindFunction("ntdll.dll", "NtQueueApcThreadEx");
    Real_NtCreateProcess = (pNtCreateProcess)DetourFindFunction("ntdll.dll", "NtCreateProcess");
    Real_NtCreateThreadEx = (pNtCreateThreadEx)DetourFindFunction("ntdll.dll", "NtCreateThreadEx");
    Real_NtOpenProcess = (pNtOpenProcess)DetourFindFunction("ntdll.dll", "NtOpenProcess");
    Real_NtLoadDriver = (pNtLoadDriver)DetourFindFunction("ntdll.dll", "NtLoadDriver");
    Real_NtCreateNamedPipeFile = (pNtCreateNamedPipeFile)DetourFindFunction("ntdll.dll", "NtCreateNamedPipeFile");
    Real_NtCreateSection = (pNtCreateSection)DetourFindFunction("ntdll.dll", "NtCreateSection");
    Real_NtCreateProcessEx = (pNtCreateProcessEx)DetourFindFunction("ntdll.dll", "NtCreateProcessEx");
    Real_NtCreateEvent = (pNtCreateEvent)DetourFindFunction("ntdll.dll", "NtCreateEvent");
    Real_NtCreateTimer = (pNtCreateTimer)DetourFindFunction("ntdll.dll", "NtCreateTimer");
    Real_NtCreateTimer2 = (pNtCreateTimer2)DetourFindFunction("ntdll.dll", "NtCreateTimer2");
    Real_NtReadVirtualMemory = (pNtReadVirtualMemory)DetourFindFunction("ntdll.dll", "NtReadVirtualMemory");
    Real_NtOpenThread = (pNtOpenThread)DetourFindFunction("ntdll.dll", "NtOpenThread");
    Real_NtWriteVirtualMemory = (t_NtWriteVirtualMemory)DetourFindFunction("ntdll.dll", "NtWriteVirtualMemory");
    Real_NtMapViewOfSection = (t_NtMapViewOfSection)DetourFindFunction("ntdll.dll", "NtMapViewOfSection");
    Real_NtAllocateVirtualMemory = (t_NtAllocateVirtualMemory)DetourFindFunction("ntdll.dll", "NtAllocateVirtualMemory");
    Real_NtProtectVirtualMemory = (t_NtProtectVirtualMemory)DetourFindFunction("ntdll.dll", "NtProtectVirtualMemory");

    DetourRestoreAfterWith();
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // All the hooks
    //DetourAttach(&(PVOID&)Real_NtSetContextThread, Catch_NtSetContextThread); // broken
    DetourAttach(&(PVOID&)Real_LdrLoadDll, Catch_LdrLoadDll);
    DetourAttach(&(PVOID&)Real_LdrGetProcedureAddress, Catch_LdrGetProcedureAddress);
    DetourAttach(&(PVOID&)Real_NtQueueApcThread, Catch_NtQueueApcThread);
    DetourAttach(&(PVOID&)Real_NtQueueApcThreadEx, Catch_NtQueueApcThreadEx);
    DetourAttach(&(PVOID&)Real_NtCreateProcess, Catch_NtCreateProcess);
    DetourAttach(&(PVOID&)Real_NtCreateThreadEx, Catch_NtCreateThreadEx);
    DetourAttach(&(PVOID&)Real_NtOpenProcess, Catch_NtOpenProcess);
    DetourAttach(&(PVOID&)Real_NtLoadDriver, Catch_NtLoadDriver);
    DetourAttach(&(PVOID&)Real_NtCreateNamedPipeFile, Catch_NtCreateNamedPipeFile);
    DetourAttach(&(PVOID&)Real_NtCreateSection, Catch_NtCreateSection);
    DetourAttach(&(PVOID&)Real_NtCreateProcessEx, Catch_NtCreateProcessEx);
    DetourAttach(&(PVOID&)Real_NtCreateEvent, Catch_NtCreateEvent);
    DetourAttach(&(PVOID&)Real_NtCreateTimer, Catch_NtCreateTimer);
    DetourAttach(&(PVOID&)Real_NtCreateTimer2, Catch_NtCreateTimer2);
    DetourAttach(&(PVOID&)Real_NtReadVirtualMemory, Catch_NtReadVirtualMemory);
    DetourAttach(&(PVOID&)Real_NtOpenThread, Catch_NtOpenThread);
    DetourAttach(&(PVOID&)Real_NtWriteVirtualMemory, Catch_NtWriteVirtualMemory);
    DetourAttach(&(PVOID&)Real_NtMapViewOfSection, Catch_NtMapViewOfSection);
    DetourAttach(&(PVOID&)Real_NtAllocateVirtualMemory, Catch_NtAllocateVirtualMemory);
    DetourAttach(&(PVOID&)Real_NtProtectVirtualMemory, Catch_NtProtectVirtualMemory);
    
    error = DetourTransactionCommit();
    if (error == NO_ERROR) {
        LOG_A(LOG_INFO, "simple" DETOURS_STRINGIFY(DETOURS_BITS) ".dll: Detoured SleepEx().\n");
    }
    else {
        LOG_A(LOG_ERROR, "simple" DETOURS_STRINGIFY(DETOURS_BITS) ".dll:Error detouring SleepEx(): %ld\n", error);
    }
    SendDllPipe(stop_str);

    return 0;
}




BOOL WINAPI DllMain(HINSTANCE hinst, DWORD dwReason, LPVOID reserved)
{
    LONG error;
    (void)hinst;
    (void)reserved;

    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    if (dwReason == DLL_PROCESS_ATTACH) {
        InitHooksThread(NULL);
    }
    else if (dwReason == DLL_PROCESS_DETACH) {
       /* DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)TrueSleepEx, TimedSleepEx);
        error = DetourTransactionCommit();

        printf("simple" DETOURS_STRINGIFY(DETOURS_BITS) ".dll:"
            " Removed SleepEx() (result=%ld), slept %ld ticks.\n", error, dwSlept);
        fflush(stdout);
        */
    }
    return TRUE;
}