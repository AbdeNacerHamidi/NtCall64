/*******************************************************************************
*
*  (C) COPYRIGHT AUTHORS, 2016 - 2017
*
*  TITLE:       FUZZWIN32K.C
*
*  VERSION:     1.20
*
*  DATE:        28 July 2017
*
*  Shadow table fuzzing routines.
*
* THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
* ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED
* TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
* PARTICULAR PURPOSE.
*
*******************************************************************************/
#include "main.h"
#include "fuzz.h"
#include "fuzzwin32k.h"
#include "tables.h"

#define WIN32K_CFG_FILE "win32k_cfg.ini"

CHAR **g_lpWin32pServiceTableNames = NULL;

BADCALLS g_Win32kSyscallBlacklist;

/*
* find_w32pservicetable
*
* Purpose:
*
* Locate shadow table info in mapped win32k copy.
*
*/
BOOL find_w32pservicetable(
    HMODULE			    MappedImageBase,
    PRAW_SERVICE_TABLE	ServiceTable
)
{
    PULONG ServiceLimit;

    ServiceLimit = (ULONG*)GetProcAddress(MappedImageBase, "W32pServiceLimit");
    if (ServiceLimit == NULL)
        return FALSE;

    ServiceTable->CountOfEntries = *ServiceLimit;
    ServiceTable->StackArgumentTable = (PBYTE)GetProcAddress(MappedImageBase, "W32pArgumentTable");
    if (ServiceTable->StackArgumentTable == NULL)
        return FALSE;

    ServiceTable->ServiceTable = (LPVOID *)GetProcAddress(MappedImageBase, "W32pServiceTable");
    if (ServiceTable->ServiceTable == NULL)
        return FALSE;

    return TRUE;
}

/*
* win32k_callproc
*
* Purpose:
*
* Handler for fuzzing thread.
*
*/
DWORD WINAPI win32k_callproc(
    PVOID Parameter
)
{
    ULONG  r;
    CALL_PARAM *CallParam = (PCALL_PARAM)Parameter;

    for (r = 0; r < 256 * 1024; r++) {
        gofuzz(CallParam->Syscall, CallParam->ParametersInStack);
    }

    return 0;
}

/*
* lookup_win32k_names
*
* Purpose:
*
* Build shadow table service names list.
*
*/
BOOL lookup_win32k_names(
    LPWSTR              ModuleName,
    ULONG_PTR           MappedImageBase,
    PRAW_SERVICE_TABLE  ServiceTable
)
{
    ULONG                   BuildNumber = 0, i;
    PIMAGE_NT_HEADERS       NtHeaders = RtlImageNtHeader((PVOID)MappedImageBase);
    ULONG_PTR	            Address;

    DWORD64  *pW32pServiceTable = NULL;
    DWORD    *Table = NULL;
    CHAR    **Names = NULL;
    PCHAR     pfn;
    IMAGE_IMPORT_BY_NAME *ImportEntry;

    if (!GetImageVersionInfo(ModuleName, NULL, NULL, &BuildNumber, NULL)) {
        OutputConsoleMessage("\r\nFailed to query win32k.sys version information.\r\n");
        return FALSE;
    }

    switch (BuildNumber) {

    case 7600:
    case 7601:
        if (ServiceTable->CountOfEntries != W32pServiceTableLimit_7601)
            return FALSE;
        Names = (CHAR**)W32pServiceTableNames_7601;
        break;

    case 9200:
        if (ServiceTable->CountOfEntries != W32pServiceTableLimit_9200)
            return FALSE;
        Names = (CHAR**)W32pServiceTableNames_9200;
        break;

    case 9600:
        if (ServiceTable->CountOfEntries != W32pServiceTableLimit_9600)
            return FALSE;
        Names = (CHAR**)W32pServiceTableNames_9600;
        break;

    default:
        break;
    }

    g_lpWin32pServiceTableNames = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ServiceTable->CountOfEntries * sizeof(PVOID));
    if (g_lpWin32pServiceTableNames == NULL)
        return FALSE;

    pW32pServiceTable = (DWORD64*)ServiceTable->ServiceTable;

    for (i = 0; i < ServiceTable->CountOfEntries; i++) {

        if (BuildNumber > 9600) {
            if (BuildNumber > 10586) {
                Table = (DWORD *)pW32pServiceTable;
                pfn = (PCHAR)(Table[i] + MappedImageBase);
            }
            else {
                pfn = (PCHAR)(pW32pServiceTable[i] - NtHeaders->OptionalHeader.ImageBase + MappedImageBase);
            }
            Address = MappedImageBase + *(ULONG_PTR*)(pfn + 6 + *(DWORD*)(pfn + 2));
            if (Address) {
                ImportEntry = (IMAGE_IMPORT_BY_NAME *)Address;
                g_lpWin32pServiceTableNames[i] = ImportEntry->Name;
            }
        }
        else {
            g_lpWin32pServiceTableNames[i] = Names[i];
        }
    }
    return TRUE;
}

/*
* fuzz_win32k
*
* Purpose:
*
* Launch win32k shadow service table fuzzing using new single thread.
*
*/
void fuzz_win32k()
{
    BOOL        bSkip = FALSE, bCond = FALSE;
    ULONG       r, c;
    HMODULE     hUser32 = 0;
    ULONG_PTR   KernelImage = 0;
    HANDLE      hCallThread = NULL;
    CALL_PARAM  CallParam;
    CHAR       *Name;
    CHAR        textbuf[1024];

    RAW_SERVICE_TABLE	ServiceTable;
    WCHAR               szBuffer[MAX_PATH * 2];

    do {
        hUser32 = LoadLibrary(TEXT("user32.dll"));
        if (hUser32 == 0)
            break;

        RtlSecureZeroMemory(szBuffer, sizeof(szBuffer));
        if (!GetSystemDirectory(szBuffer, MAX_PATH))
            break;

        _strcat(szBuffer, TEXT("\\win32k.sys"));
        KernelImage = (ULONG_PTR)LoadLibraryEx(szBuffer, NULL, 0);
        if (KernelImage == 0)
            break;

        RtlSecureZeroMemory(&g_Win32kSyscallBlacklist, sizeof(g_Win32kSyscallBlacklist));
        ReadBlacklistCfg(&g_Win32kSyscallBlacklist, CFG_FILE, "win32k");

        if (!find_w32pservicetable((HMODULE)KernelImage, &ServiceTable))
            break;

        if (!lookup_win32k_names(szBuffer, KernelImage, &ServiceTable))
            break;

        force_priv();

        for (c = 0; c < ServiceTable.CountOfEntries; c++) {

            Name = g_lpWin32pServiceTableNames[c];

            _strcpy_a(textbuf, "sid ");
            ultostr_a(c + W32SYSCALLSTART, _strend_a(textbuf));

            _strcat_a(textbuf, ", args(stack): ");
            ultostr_a(ServiceTable.StackArgumentTable[c] / 4, _strend_a(textbuf));

            _strcat_a(textbuf, "\tname:");
            if (Name != NULL)
                _strcat_a(textbuf, Name);
            else
                _strcat_a(textbuf, "#noname#");

            bSkip = SyscallBlacklisted(Name, &g_Win32kSyscallBlacklist);
            if (bSkip) {
                _strcat_a(textbuf, " ******* found in blacklist, skip");
            }

            _strcat_a(textbuf, "\r\n");
            OutputConsoleMessage(textbuf);

            if (bSkip)
                continue;

            CallParam.ParametersInStack = ServiceTable.StackArgumentTable[c];
            CallParam.Syscall = c + W32SYSCALLSTART;
            hCallThread = CreateThread(NULL, 0, win32k_callproc, (LPVOID)&CallParam, 0, &r);
            if (hCallThread) {
                if (WaitForSingleObject(hCallThread, 10 * 1000) == WAIT_TIMEOUT) {
                    _strcpy_a(textbuf, "Timeout reached for callproc of Service: ");
                    ultostr_a(CallParam.Syscall, _strend_a(textbuf));
                    _strcat_a(textbuf, "\r\n");
                    OutputConsoleMessage(textbuf);
                    TerminateThread(hCallThread, (DWORD)-1);
                }
                CloseHandle(hCallThread);
            }
        }

    } while (bCond);

    if (KernelImage != 0) FreeLibrary((HMODULE)KernelImage);
    if (hUser32 != 0) FreeLibrary(hUser32);

    OutputConsoleMessage("Win32k services fuzzing complete.\r\n");
}
