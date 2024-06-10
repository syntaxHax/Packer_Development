// Credit: https://github.com/aaaddress1/RunPE-In-Memory/tree/master

#include <windows.h> 
#include <stdio.h>
#include <tchar.h>

#ifdef VERBOSE
#define DBG(...) printf(__VA_ARGS__ "\n")
#else
#define DBG(...)
#endif

BOOL hijackCmdline = FALSE;
char* sz_masqCmd_Ansi = NULL, *sz_masqCmd_ArgvAnsi[100] = {  };
wchar_t* sz_masqCmd_Widh = NULL, *sz_masqCmd_ArgvWidh[100] = { };
int int_masqCmd_Argc = 0;
LPWSTR hookGetCommandLineW() { return sz_masqCmd_Widh; }
LPSTR hookGetCommandLineA() { return sz_masqCmd_Ansi;  }

typedef struct _BASE_RELOCATION_ENTRY {
    WORD Offset : 12;
    WORD Type : 4;
} BASE_RELOCATION_ENTRY;

#define RELOC_32BIT_FIELD 3

BYTE* 
getNtHdrs(
    BYTE *pe_buffer
)
{
    if (pe_buffer == NULL) return NULL;

    IMAGE_DOS_HEADER *idh = (IMAGE_DOS_HEADER*)pe_buffer;
    if (idh->e_magic != IMAGE_DOS_SIGNATURE) {
        return NULL;
    }
    const LONG kMaxOffset = 1024;
    LONG pe_offset = idh->e_lfanew;
    if (pe_offset > kMaxOffset) return NULL;
    IMAGE_NT_HEADERS32 *inh = (IMAGE_NT_HEADERS32 *)((BYTE*)pe_buffer + pe_offset);
    if (inh->Signature != IMAGE_NT_SIGNATURE) return NULL;
    return (BYTE*)inh;
}

IMAGE_DATA_DIRECTORY* 
getPeDir(
    PVOID pe_buffer, 
    SIZE_T dir_id
)
{
    if (dir_id >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return NULL;

    BYTE* nt_headers = getNtHdrs((BYTE*)pe_buffer);
    if (nt_headers == NULL) return NULL;

    IMAGE_DATA_DIRECTORY* peDir = NULL;

    IMAGE_NT_HEADERS* nt_header = (IMAGE_NT_HEADERS*)nt_headers;
    peDir = &(nt_header->OptionalHeader.DataDirectory[dir_id]);

    if (!peDir->VirtualAddress) {
        return NULL;
    }
    return peDir;
}

BOOL 
applyReloc(
    ULONGLONG newBase, 
    ULONGLONG oldBase, 
    PVOID modulePtr, 
    SIZE_T moduleSize
)
{
    IMAGE_DATA_DIRECTORY* relocDir = getPeDir(modulePtr, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (relocDir == NULL) /* Cannot relocate - application have no relocation table */
        return FALSE;

    SIZE_T maxSize = relocDir->Size;
    SIZE_T relocAddr = relocDir->VirtualAddress;
    IMAGE_BASE_RELOCATION* reloc = NULL;

    SIZE_T parsedSize = 0;
    for (; parsedSize < maxSize; parsedSize += reloc->SizeOfBlock) {
        reloc = (IMAGE_BASE_RELOCATION*)(relocAddr + parsedSize + (SIZE_T)(modulePtr));
        if (!reloc->VirtualAddress || reloc->SizeOfBlock == 0)
            break;

        SIZE_T entriesNum = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(BASE_RELOCATION_ENTRY);
        SIZE_T page = reloc->VirtualAddress;

        BASE_RELOCATION_ENTRY* entry = (BASE_RELOCATION_ENTRY*)((SIZE_T)(reloc) + sizeof(IMAGE_BASE_RELOCATION));
        for (SIZE_T i = 0; i < entriesNum; i++) {
            SIZE_T offset = entry->Offset;
            SIZE_T type = entry->Type;
            SIZE_T reloc_field = page + offset;
            if (entry == NULL || type == 0)
                break;
            if (type != RELOC_32BIT_FIELD) {
                return FALSE;
            }
            if (reloc_field >= moduleSize) {
                return FALSE;
            }

            SIZE_T* relocateAddr = (SIZE_T*)((SIZE_T)(modulePtr) + reloc_field);
            (*relocateAddr) = ((*relocateAddr) - oldBase + newBase);
            entry = (BASE_RELOCATION_ENTRY*)((SIZE_T)(entry) + sizeof(BASE_RELOCATION_ENTRY));
        }
    }
    return (parsedSize != 0);
}

int __wgetmainargs(int* _Argc, wchar_t*** _Argv, wchar_t*** _Env, int _useless_, void* _useless) {
    *_Argc = int_masqCmd_Argc;
    *_Argv = (wchar_t **)sz_masqCmd_ArgvWidh;
    return 0;
}
int __getmainargs(int* _Argc, char*** _Argv, char*** _Env, int _useless_, void* _useless) {
    *_Argc = int_masqCmd_Argc;
    *_Argv = (char **)sz_masqCmd_ArgvAnsi;
    return 0;
}

BOOL 
fixIAT(
    PVOID modulePtr
)
{
    DBG("    [+] Fix Import Address Table");
    IMAGE_DATA_DIRECTORY *importsDir = getPeDir(modulePtr, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (importsDir == NULL) return FALSE;

    SIZE_T maxSize = importsDir->Size;
    SIZE_T impAddr = importsDir->VirtualAddress;

    IMAGE_IMPORT_DESCRIPTOR* lib_desc = NULL;
    SIZE_T parsedSize = 0;

    for (; parsedSize < maxSize; parsedSize += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        lib_desc = (IMAGE_IMPORT_DESCRIPTOR*)(impAddr + parsedSize + (ULONG_PTR)modulePtr);

        if (!lib_desc->OriginalFirstThunk && !lib_desc->FirstThunk) break;
        LPSTR lib_name = (LPSTR)((ULONGLONG)modulePtr + lib_desc->Name);

        SIZE_T call_via = lib_desc->FirstThunk;
        SIZE_T thunk_addr = lib_desc->OriginalFirstThunk;
        if (!thunk_addr) thunk_addr = lib_desc->FirstThunk;

        SIZE_T offsetField = 0;
        SIZE_T offsetThunk = 0;
        while (TRUE)
        {
            IMAGE_THUNK_DATA* fieldThunk = (IMAGE_THUNK_DATA*)((SIZE_T)(modulePtr) + offsetField + call_via);
            IMAGE_THUNK_DATA* orginThunk = (IMAGE_THUNK_DATA*)((SIZE_T)(modulePtr) + offsetThunk + thunk_addr);

            if (orginThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32 || orginThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) // check if using ordinal (both x86 && x64)
            {
                SIZE_T addr = (SIZE_T)GetProcAddress(LoadLibraryA(lib_name), (char *)(orginThunk->u1.Ordinal & 0xFFFF));
                fieldThunk->u1.Function = addr;
            }
            
            if (!fieldThunk->u1.Function) break;

            if (fieldThunk->u1.Function == orginThunk->u1.Function) {
                
                PIMAGE_IMPORT_BY_NAME by_name = (PIMAGE_IMPORT_BY_NAME)((SIZE_T)(modulePtr) + orginThunk->u1.AddressOfData);

                LPSTR func_name = (LPSTR)by_name->Name;
                SIZE_T addr = (SIZE_T)GetProcAddress(LoadLibraryA(lib_name), func_name);

                if (hijackCmdline && strcmpi(func_name, "GetCommandLineA") == 0)
                    fieldThunk->u1.Function = (SIZE_T)hookGetCommandLineA;
                else if (hijackCmdline && strcmpi(func_name, "GetCommandLineW") == 0)
                    fieldThunk->u1.Function = (SIZE_T)hookGetCommandLineW;
                else if (hijackCmdline && strcmpi(func_name, "__wgetmainargs") == 0)
                    fieldThunk->u1.Function = (SIZE_T)__wgetmainargs;
                else if (hijackCmdline && strcmpi(func_name, "__getmainargs") == 0)
                    fieldThunk->u1.Function = (SIZE_T)__getmainargs;
                else
                    fieldThunk->u1.Function = addr;

            }
            offsetField += sizeof(IMAGE_THUNK_DATA);
            offsetThunk += sizeof(IMAGE_THUNK_DATA);
        }
    }
    return TRUE;
}

INT 
RunPortableExecutable(
    PVOID data, 
    HMODULE hNtdll
)
{
    IMAGE_NT_HEADERS *ntHeader = (IMAGE_NT_HEADERS *)getNtHdrs(data);
    if (!ntHeader) 
    {
        return FALSE;
    }

    IMAGE_DATA_DIRECTORY* relocDir = getPeDir(data, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    LPVOID preferAddr = (LPVOID)ntHeader->OptionalHeader.ImageBase;

    ((int(WINAPI*)(HANDLE, PVOID))GetProcAddress(hNtdll, "NtUnmapViewOfSection"))((HANDLE)-1, (LPVOID)ntHeader->OptionalHeader.ImageBase);
    
    BYTE* pImageBase = (BYTE *)VirtualAlloc(preferAddr, ntHeader->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pImageBase && !relocDir)
    {
        return FALSE;
    }
    if (!pImageBase && relocDir)
    {
        pImageBase = (BYTE *)VirtualAlloc(NULL, ntHeader->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!pImageBase)
        {
            DBG("    [-] Allocate Memory For Image Base Failure.");
            return FALSE;
        }
    }
    
    DBG("    [+] Mapping Section ...");
    ntHeader->OptionalHeader.ImageBase = (SIZE_T)pImageBase;
    memcpy(pImageBase, data, ntHeader->OptionalHeader.SizeOfHeaders);

    IMAGE_SECTION_HEADER * SectionHeaderArr = (IMAGE_SECTION_HEADER *)((SIZE_T)(ntHeader) + sizeof(IMAGE_NT_HEADERS));
    for (int i = 0; i < ntHeader->FileHeader.NumberOfSections; i++)
    {
        memcpy((LPVOID)((SIZE_T)(pImageBase) + SectionHeaderArr[i].VirtualAddress), (LPVOID)((SIZE_T)(data) + SectionHeaderArr[i].PointerToRawData), SectionHeaderArr[i].SizeOfRawData);
    }

    fixIAT(pImageBase);

    if (pImageBase != preferAddr) 
        if (applyReloc((SIZE_T)pImageBase, (SIZE_T)preferAddr, pImageBase, ntHeader->OptionalHeader.SizeOfImage))
            DBG("    [+] Relocation Fixed.");
    SIZE_T retAddr = (SIZE_T)(pImageBase)+ntHeader->OptionalHeader.AddressOfEntryPoint;

    DBG("    [+] Executing PE ...");
    ((void(*)())retAddr)();
}