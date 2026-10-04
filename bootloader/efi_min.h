// bootloader/efi_min.h
// Definiciones mínimas de UEFI para el bootloader de Aurora OS.
// Autocontenido: no depende de gnu-efi ni de ninguna librería externa.
// Compatible con clang -target x86_64-pc-windows-msvc (MS ABI nativo).
#ifndef BOOTLOADER_EFI_MIN_H
#define BOOTLOADER_EFI_MIN_H

#include <stdint.h>
#include <stddef.h>

#if __SIZEOF_WCHAR_T__ != 2
#error "Este fichero requiere -fshort-wchar (wchar_t de 16 bits)"
#endif

/* ---- Tipos base ---- */
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef char      CHAR8;
typedef uint16_t  CHAR16;
typedef uint8_t   BOOLEAN;
typedef uint64_t  UINTN;
typedef int64_t   INTN;
typedef void      VOID;

#ifndef TRUE
#define TRUE  ((BOOLEAN)1)
#endif
#ifndef FALSE
#define FALSE ((BOOLEAN)0)
#endif

typedef UINT64 EFI_STATUS;
typedef VOID*  EFI_HANDLE;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

/* ---- EFI_STATUS helpers ---- */
#define EFI_SUCCESS               0ULL
#define EFI_LOAD_ERROR            0x8000000000000001ULL
#define EFI_INVALID_PARAMETER     0x8000000000000002ULL
#define EFI_UNSUPPORTED           0x8000000000000003ULL
#define EFI_BUFFER_TOO_SMALL      0x8000000000000005ULL
#define EFI_NOT_READY             0x8000000000000006ULL
#define EFI_DEVICE_ERROR          0x8000000000000007ULL
#define EFI_NOT_FOUND             0x800000000000000EULL

#define EFI_ERROR(status) (((INT64)(status)) < 0)

/* ---- GUID ---- */
typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Memoria ---- */
typedef UINT32 EFI_MEMORY_TYPE;
#define EfiReservedMemoryType    0
#define EfiLoaderCode            1
#define EfiLoaderData            2
#define EfiBootServicesCode      3
#define EfiBootServicesData      4
#define EfiRuntimeServicesCode   5
#define EfiRuntimeServicesData   6
#define EfiConventionalMemory    7
#define EfiUnusableMemory        8
#define EfiACPIReclaimMemory     9
#define EfiACPIMemoryNVS         10
#define EfiMemoryMappedIO        11
#define EfiMemoryMappedIOPortSpace 12
#define EfiPalCode               13
#define EfiPersistentMemory      14
#define EfiMaxMemoryType         15

typedef UINT32 EFI_ALLOCATE_TYPE;
#define AllocateAnyPages     0
#define AllocateMaxAddress   1
#define AllocateAddress      2
#define MaxAllocateType      3

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    EFI_GUID VendorGuid;
    VOID     *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ---- Simple Text Output Protocol ---- */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (*EFI_TEXT_RESET_FN)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (*EFI_TEXT_STRING_FN)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET_FN       Reset;
    EFI_TEXT_STRING_FN      OutputString;
    VOID                   *TestString;
    VOID                   *QueryMode;
    VOID                   *SetMode;
    VOID                   *SetAttribute;
    VOID                   *ClearScreen;
    VOID                   *SetCursorPosition;
    VOID                   *EnableCursor;
    VOID                   *Mode;
};

/* ---- Loaded Image Protocol ---- */
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    VOID *SystemTable;
    EFI_HANDLE DeviceHandle;
    VOID *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- File Protocol ---- */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef EFI_FILE_PROTOCOL EFI_FILE;

typedef EFI_STATUS (*EFI_FILE_OPEN_FN)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
typedef EFI_STATUS (*EFI_FILE_CLOSE_FN)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (*EFI_FILE_READ_FN)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (*EFI_FILE_SET_POSITION_FN)(EFI_FILE_PROTOCOL *This, UINT64 Position);

struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_FILE_OPEN_FN  Open;
    EFI_FILE_CLOSE_FN Close;
    VOID *Delete;
    EFI_FILE_READ_FN  Read;
    VOID *Write;
    VOID *GetPosition;
    EFI_FILE_SET_POSITION_FN SetPosition;
    VOID *GetInfo;
    VOID *SetInfo;
    VOID *Flush;
    VOID *OpenEx;
    VOID *ReadEx;
    VOID *WriteEx;
    VOID *FlushEx;
};

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

/* ---- Simple File System Protocol ---- */
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS (*EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
};

/* ---- Graphics Output Protocol ---- */
typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    struct { UINT32 RedMask, GreenMask, BlueMask, ReservedMask; } PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    VOID *QueryMode;
    VOID *SetMode;
    VOID *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- Boot Services typedefs ---- */
typedef EFI_STATUS (*EFI_ALLOCATE_PAGES_FN)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP_FN)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (*EFI_ALLOCATE_POOL_FN)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
typedef EFI_STATUS (*EFI_FREE_POOL_FN)(VOID *Buffer);
typedef EFI_STATUS (*EFI_STALL_FN)(UINTN Microseconds);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES_FN)(EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL_FN)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
typedef EFI_STATUS (*EFI_OPEN_PROTOCOL_FN)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface, EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, UINT32 Attributes);
typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL_FN)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);

/* ---- Boot Services ----
 *
 * IMPORTANTE: el orden de los miembros coincide con el layout UEFI 2.x.
 * Los offsets importan para la ABI con OVMF. Los VOID* son funciones que
 * no usamos (o no tenemos tipo definido aún). Solo tipamos las que
 * llamamos: AllocatePages, GetMemoryMap, AllocatePool, FreePool,
 * ExitBootServices, LocateProtocol, OpenProtocol, HandleProtocol, Stall.
 */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    EFI_ALLOCATE_PAGES_FN    AllocatePages;
    VOID *FreePages;
    EFI_GET_MEMORY_MAP_FN    GetMemoryMap;
    EFI_ALLOCATE_POOL_FN     AllocatePool;
    EFI_FREE_POOL_FN         FreePool;      // [FIX] era VOID*
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL_FN   HandleProtocol;
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_EXIT_BOOT_SERVICES_FN ExitBootServices;
    VOID                    *GetNextMonotonicCount;
    EFI_STALL_FN             Stall;         // [FIX] era VOID*
    VOID *SetWatchdogTimer;
    VOID *ConnectController;
    VOID *DisconnectController;
    EFI_OPEN_PROTOCOL_FN     OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;
    VOID *ProtocolsPerHandle;
    VOID *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL_FN   LocateProtocol;
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;
    VOID *CalculateCrc32;
    VOID *CopyMem;
    VOID *SetMem;
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

/* ---- System Table ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    VOID *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    VOID *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Constantes adicionales ---- */
#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x01
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL       0x02

/* ---- Globals ---- */
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_HANDLE gImageHandle;

/* ---- GUIDs (definidos en efi_main.c) ---- */
extern const EFI_GUID gEfiLoadedImageProtocolGuid;
extern const EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern const EFI_GUID gEfiGraphicsOutputProtocolGuid;

/* ---- Helpers (definidos en efi_main.c) ---- */
void  zero_mem(VOID *dst, UINTN n);
void  copy_mem(VOID *dst, const VOID *src, UINTN n);
int   compare_mem(const VOID *a, const VOID *b, UINTN n);
int   str_cmp_16(const CHAR16 *a, const CHAR16 *b);
void  efi_print(const CHAR16 *fmt, ...);

#endif /* BOOTLOADER_EFI_MIN_H */