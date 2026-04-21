#include "winvfs.hpp"
#include <Windows.h>
#include "wchar_util.h"
#include "fileop.h"
#include "detours.h"
#include <functional>
// #include <ntstatus.h>

#if WINVFS_LOGGING
    #define LOG(...) Log(__VA_ARGS__)
    #define GLOG(...) g_vfs.Log(__VA_ARGS__)
#else
    #define LOG(...) ((void)0)
    #define GLOG(...) ((void)0)
#endif

static VFS g_vfs;

VFS& GetGlobalVFS() {
    return g_vfs;
}

#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006L)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)

std::string getFinalPath(HANDLE hDir, DWORD flags) {
    wchar_t buf[MAX_PATH];
    DWORD ret = GetFinalPathNameByHandleW(hDir, buf, MAX_PATH, flags);
    std::wstring result;
    if (ret == 0) {
        return "";
    }
    if (ret < MAX_PATH) {
        result.assign(buf, ret);
    } else {
        wchar_t* tbuf = new wchar_t[ret + 1];
        ret = GetFinalPathNameByHandleW(hDir, tbuf, ret + 1, flags);
        if (ret == 0) {
            delete[] tbuf;
            return "";
        }
        result.assign(tbuf, ret);
        delete[] tbuf;
    }
    std::string strResult;
    wchar_util::wstr_to_str(strResult, result, CP_UTF8);
    return strResult;
}

static decltype(&NtCreateFile) Real_NtCreateFile = nullptr;
static decltype(&NtClose) Real_NtClose = nullptr;
typedef NTSTATUS(NTAPI* NtReadFile_t)(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID Buffer,
    IN ULONG Length,
    IN PLARGE_INTEGER ByteOffset OPTIONAL,
    IN PULONG Key OPTIONAL
);
static NtReadFile_t Real_NtReadFile = nullptr;
typedef NTSTATUS(NTAPI* NtQueryInformationFile_t)(
    IN HANDLE FileHandle,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass
);
static NtQueryInformationFile_t Real_NtQueryInformationFile = nullptr;
#define FileBasicInformation 0x4
typedef struct _FILE_BASIC_INFORMATION {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG         FileAttributes;
} FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;
#define FileStandardInformation 0x5
typedef struct _FILE_STANDARD_INFORMATION {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG         NumberOfLinks;
    BOOLEAN       DeletePending;
    BOOLEAN       Directory;
} FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;
#define FileInternalInformation 0x6
typedef struct _FILE_INTERNAL_INFORMATION {
    LARGE_INTEGER IndexNumber;
} FILE_INTERNAL_INFORMATION, *PFILE_INTERNAL_INFORMATION;
#define FileEaInformation 0x7
typedef struct _FILE_EA_INFORMATION {
    ULONG EaSize;
} FILE_EA_INFORMATION, *PFILE_EA_INFORMATION;
#define FileAccessInformation 0x8
typedef struct _FILE_ACCESS_INFORMATION {
    ACCESS_MASK AccessFlags;
} FILE_ACCESS_INFORMATION, *PFILE_ACCESS_INFORMATION;
#define FilePositionInformation 14
typedef struct _FILE_POSITION_INFORMATION {
    LARGE_INTEGER CurrentByteOffset;
} FILE_POSITION_INFORMATION, *PFILE_POSITION_INFORMATION;
#define FileModeInformation 16
typedef struct _FILE_MODE_INFORMATION {
    ULONG Mode;
} FILE_MODE_INFORMATION, *PFILE_MODE_INFORMATION;
#define FILE_BYTE_ALIGNMENT 0x00000000
#define FileAlignmentInformation 17
typedef struct _FILE_ALIGNMENT_INFORMATION {
    ULONG AlignmentRequirement;
} FILE_ALIGNMENT_INFORMATION, *PFILE_ALIGNMENT_INFORMATION;
#define FileNameInformation 9
#define FileNormalizedNameInformation 0x30
typedef struct _FILE_NAME_INFORMATION {
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_NAME_INFORMATION, *PFILE_NAME_INFORMATION;
#define FileAllInformation 18
typedef struct _FILE_ALL_INFORMATION {
    FILE_BASIC_INFORMATION     BasicInformation;
    FILE_STANDARD_INFORMATION  StandardInformation;
    FILE_INTERNAL_INFORMATION  InternalInformation;
    FILE_EA_INFORMATION        EaInformation;
    FILE_ACCESS_INFORMATION    AccessInformation;
    FILE_POSITION_INFORMATION  PositionInformation;
    FILE_MODE_INFORMATION      ModeInformation;
    FILE_ALIGNMENT_INFORMATION AlignmentInformation;
    FILE_NAME_INFORMATION      NameInformation;
} FILE_ALL_INFORMATION, *PFILE_ALL_INFORMATION;
#define FileCompletionInformation 30
typedef struct _FILE_COMPLETION_INFORMATION {
    HANDLE Port;
    PVOID  Key;
} FILE_COMPLETION_INFORMATION, *PFILE_COMPLETION_INFORMATION;
#define FileIoCompletionNotificationInformation 41
typedef struct _FILE_IO_COMPLETION_NOTIFICATION_INFORMATION {
    ULONG Flags;
} FILE_IO_COMPLETION_NOTIFICATION_INFORMATION, *PFILE_IO_COMPLETION_NOTIFICATION_INFORMATION;
typedef enum _FSINFOCLASS {
    FileFsVolumeInformation          = 1,
    FileFsLabelInformation,         // 2
    FileFsSizeInformation,          // 3
    FileFsDeviceInformation,        // 4
    FileFsAttributeInformation,     // 5
    FileFsControlInformation,       // 6
    FileFsFullSizeInformation,      // 7
    FileFsObjectIdInformation,      // 8
    FileFsDriverPathInformation,    // 9
    FileFsVolumeFlagsInformation,   // 10
    FileFsSectorSizeInformation,    // 11
    FileFsDataCopyInformation,      // 12
    FileFsMetadataSizeInformation,  // 13
    FileFsMaximumInformation
} FS_INFORMATION_CLASS, *PFS_INFORMATION_CLASS;
typedef NTSTATUS(NTAPI* NtQueryVolumeInformationFile_t)(
    IN HANDLE FileHandle,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FsInformation,
    IN ULONG Length,
    IN FS_INFORMATION_CLASS FsInformationClass
);
static NtQueryVolumeInformationFile_t Real_NtQueryVolumeInformationFile = nullptr;
typedef struct _FILE_FS_VOLUME_INFORMATION {
    LARGE_INTEGER VolumeCreationTime;
    ULONG         VolumeSerialNumber;
    ULONG         VolumeLabelLength;
    BOOLEAN       SupportsObjects;
    WCHAR         VolumeLabel[1];
} FILE_FS_VOLUME_INFORMATION, *PFILE_FS_VOLUME_INFORMATION;
typedef struct _FILE_FS_DEVICE_INFORMATION {
    DEVICE_TYPE DeviceType;
    ULONG       Characteristics;
} FILE_FS_DEVICE_INFORMATION, *PFILE_FS_DEVICE_INFORMATION;
static decltype(&NtQueryObject) Real_NtQueryObject = nullptr;
#define ObjectNameInformation 1
typedef struct _OBJECT_NAME_INFORMATION {
    UNICODE_STRING Name;
} OBJECT_NAME_INFORMATION, *POBJECT_NAME_INFORMATION;
typedef NTSTATUS(NTAPI* NtQueryAttributesFile_t)(
    IN POBJECT_ATTRIBUTES ObjectAttributes,
    OUT PFILE_BASIC_INFORMATION FileInformation
);
static NtQueryAttributesFile_t Real_NtQueryAttributesFile = nullptr;
typedef NTSTATUS(NTAPI* NtSetInformationFile_t)(
    IN HANDLE FileHandle,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass
);
static NtSetInformationFile_t Real_NtSetInformationFile = nullptr;
static decltype(&NtOpenFile) Real_NtOpenFile = nullptr;
typedef NTSTATUS(NTAPI* NtCreateSection_t)(
    OUT PHANDLE SectionHandle,
    IN ACCESS_MASK DesiredAccess,
    IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
    IN PLARGE_INTEGER MaximumSize OPTIONAL,
    IN ULONG SectionPageProtection,
    IN ULONG AllocationAttributes,
    IN HANDLE FileHandle OPTIONAL
);
static NtCreateSection_t Real_NtCreateSection = nullptr;
/**
 * The SECTION_INHERIT structure specifies how the mapped view of the section is to be shared with child processes.
 */
typedef enum _SECTION_INHERIT {
    ViewShare = 1, // The mapped view of the section will be mapped into any child processes created by the process.
    ViewUnmap = 2  // The mapped view of the section will not be mapped into any child processes created by the process.
} SECTION_INHERIT;
typedef NTSTATUS(NTAPI* NtMapViewOfSection_t)(
    IN HANDLE SectionHandle,
    IN HANDLE ProcessHandle,
    IN OUT PVOID* BaseAddress,
    IN ULONG_PTR ZeroBits,
    IN SIZE_T CommitSize,
    IN OUT PLARGE_INTEGER SectionOffset OPTIONAL,
    IN OUT PSIZE_T ViewSize,
    IN SECTION_INHERIT InheritDisposition,
    IN ULONG AllocationType,
    IN ULONG Win32Protect
);
static NtMapViewOfSection_t Real_NtMapViewOfSection = nullptr;
typedef NTSTATUS(NTAPI* NtUnmapViewOfSection_t)(
    IN HANDLE ProcessHandle,
    IN PVOID BaseAddress
);
static NtUnmapViewOfSection_t Real_NtUnmapViewOfSection = nullptr;
typedef NTSTATUS(NTAPI* NtDuplicateObject_t)(
    IN HANDLE SourceProcessHandle,
    IN HANDLE SourceHandle,
    IN HANDLE TargetProcessHandle OPTIONAL,
    OUT PHANDLE TargetHandle OPTIONAL,
    IN ACCESS_MASK DesiredAccess,
    IN ULONG HandleAttributes,
    IN ULONG Options
);
static NtDuplicateObject_t Real_NtDuplicateObject = nullptr;
typedef struct _FILE_NETWORK_OPEN_INFORMATION {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG         FileAttributes;
} FILE_NETWORK_OPEN_INFORMATION, *PFILE_NETWORK_OPEN_INFORMATION;
typedef NTSTATUS(NTAPI* NtQueryFullAttributesFile_t)(
    IN POBJECT_ATTRIBUTES ObjectAttributes,
    OUT PFILE_NETWORK_OPEN_INFORMATION FileInformation
);
static NtQueryFullAttributesFile_t Real_NtQueryFullAttributesFile = nullptr;
typedef NTSTATUS(NTAPI* NtQueryDirectoryFile_t)(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass,
    IN BOOLEAN ReturnSingleEntry,
    IN PUNICODE_STRING FileName OPTIONAL,
    IN BOOLEAN RestartScan
);
static NtQueryDirectoryFile_t Real_NtQueryDirectoryFile = nullptr;
typedef NTSTATUS(NTAPI* NtQueryDirectoryFileEx_t)(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass,
    IN ULONG QueryFlags,
    IN PUNICODE_STRING FileName OPTIONAL
);
static NtQueryDirectoryFileEx_t Real_NtQueryDirectoryFileEx = nullptr;
#define SL_RESTART_SCAN 0x00000001
#define SL_RETURN_SINGLE_ENTRY 0x00000002
#define FileDirectoryInformation 1
typedef struct _FILE_DIRECTORY_INFORMATION {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    WCHAR         FileName[1];
} FILE_DIRECTORY_INFORMATION, *PFILE_DIRECTORY_INFORMATION;
#define FileFullDirectoryInformation 2
typedef struct _FILE_FULL_DIR_INFORMATION {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    ULONG         EaSize;
    WCHAR         FileName[1];
} FILE_FULL_DIR_INFORMATION, *PFILE_FULL_DIR_INFORMATION;
#define FileBothDirectoryInformation 3
typedef struct _FILE_BOTH_DIR_INFORMATION {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    ULONG         EaSize;
    CCHAR         ShortNameLength;
    WCHAR         ShortName[12];
    WCHAR         FileName[1];
} FILE_BOTH_DIR_INFORMATION, *PFILE_BOTH_DIR_INFORMATION;
#define FileNamesInformation 12
typedef struct _FILE_NAMES_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_NAMES_INFORMATION, *PFILE_NAMES_INFORMATION;
#define FileIdBothDirectoryInformation 37
typedef struct _FILE_ID_BOTH_DIR_INFORMATION {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    ULONG         EaSize;
    CCHAR         ShortNameLength;
    WCHAR         ShortName[12];
    LARGE_INTEGER FileId;
    WCHAR         FileName[1];
} FILE_ID_BOTH_DIR_INFORMATION, *PFILE_ID_BOTH_DIR_INFORMATION;
#define FileIdFullDirectoryInformation 38
typedef struct _FILE_ID_FULL_DIR_INFORMATION {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    ULONG         EaSize;
    LARGE_INTEGER FileId;
    WCHAR         FileName[1];
} FILE_ID_FULL_DIR_INFORMATION, *PFILE_ID_FULL_DIR_INFORMATION;
typedef BOOLEAN(NTAPI* RtlIsNameInExpression_t)(
    IN PUNICODE_STRING Expression,
    IN PUNICODE_STRING Name,
    IN BOOLEAN IgnoreCase,
    IN PWCHAR  UpcaseTable OPTIONAL
);
static RtlIsNameInExpression_t Real_RtlIsNameInExpression = nullptr;
typedef NTSTATUS(NTAPI* NtWriteFile_t)(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN PVOID Buffer,
    IN ULONG Length,
    IN PLARGE_INTEGER ByteOffset OPTIONAL,
    IN PULONG Key OPTIONAL
);
static NtWriteFile_t Real_NtWriteFile = nullptr;
typedef NTSTATUS(NTAPI* NtSetIoCompletion_t)(
    IN HANDLE IoCompletionHandle,
    IN PVOID KeyContext,
    IN PVOID ApcContext OPTIONAL,
    IN ULONG IoStatus,
    IN ULONG_PTR NumberOfBytesTransferred
);
static NtSetIoCompletion_t Real_NtSetIoCompletion = nullptr;

__kernel_entry NTSTATUS NTAPI Hooked_NtCreateFile(
    OUT PHANDLE FileHandle,
    IN ACCESS_MASK DesiredAccess,
    IN POBJECT_ATTRIBUTES ObjectAttributes,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN PLARGE_INTEGER AllocationSize OPTIONAL,
    IN ULONG FileAttributes,
    IN ULONG ShareAccess,
    IN ULONG CreateDisposition,
    IN ULONG CreateOptions,
    IN PVOID EaBuffer OPTIONAL,
    IN ULONG EaLength
) {
    if (ObjectAttributes->ObjectName && ObjectAttributes->ObjectName->Buffer) {
        std::wstring wFilepath(ObjectAttributes->ObjectName->Buffer, ObjectAttributes->ObjectName->Length / sizeof(WCHAR));
        std::string filepath;
        if (wchar_util::wstr_to_str(filepath, wFilepath, CP_UTF8)) {
            if (ObjectAttributes->RootDirectory) {
                auto hDir = ObjectAttributes->RootDirectory;
                std::string basePath = getFinalPath(hDir, FILE_NAME_NORMALIZED);
                filepath = basePath + "\\" + filepath;
            }
            GLOG("NtCreateFile: %s\n", filepath.c_str());
            FileEntry entry;
            Xp3Archive* archive;
            std::string directoryName;
            if (g_vfs.GetEntry(filepath, entry, archive)) {
                GLOG("File found in VFS: %s\n", filepath.c_str());
                auto hFile = g_vfs.OpenFile(entry, archive);
                if (hFile != INVALID_HANDLE_VALUE) {
                    *FileHandle = hFile;
                    IoStatusBlock->Status = FILE_OPENED;
                    IoStatusBlock->Information = 0;
                    GLOG("File opened successfully: %s. %p\n", filepath.c_str(), hFile);
                    return STATUS_SUCCESS;
                }
            } else if (g_vfs.IsRootDirectory(filepath)) {
                auto status = Real_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
                if (status == STATUS_SUCCESS) {
                    g_vfs.AddExistedDirHandle(*FileHandle, "/");
                }
                return status;
            } else if (g_vfs.GetDirectoryName(filepath, directoryName)) {
                GLOG("Directory found in VFS: %s\n", filepath.c_str());
                auto status = Real_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
                if (status == STATUS_SUCCESS) {
                    g_vfs.AddExistedDirHandle(*FileHandle, directoryName);
                    return status;
                }
                HANDLE hDir = g_vfs.OpenDirectory(directoryName);
                if (hDir == INVALID_HANDLE_VALUE) {
                    return STATUS_UNSUCCESSFUL;
                }
                *FileHandle = hDir;
                IoStatusBlock->Status = FILE_OPENED;
                IoStatusBlock->Information = 0;
                GLOG("Directory opened successfully: %s. %p\n", filepath.c_str(), hDir);
                return STATUS_SUCCESS;
            }
        }
    }
    return Real_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtOpenFile(
    OUT PHANDLE FileHandle,
    IN ACCESS_MASK DesiredAccess,
    IN POBJECT_ATTRIBUTES ObjectAttributes,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN ULONG ShareAccess,
    IN ULONG OpenOptions
) {
    if (ObjectAttributes->ObjectName && ObjectAttributes->ObjectName->Buffer) {
        std::wstring wFilepath(ObjectAttributes->ObjectName->Buffer, ObjectAttributes->ObjectName->Length / sizeof(WCHAR));
        std::string filepath;
        if (wchar_util::wstr_to_str(filepath, wFilepath, CP_UTF8)) {
            if (ObjectAttributes->RootDirectory) {
                auto hDir = ObjectAttributes->RootDirectory;
                std::string basePath = getFinalPath(hDir, FILE_NAME_NORMALIZED);
                filepath = basePath + "\\" + filepath;
            }
            GLOG("NtOpenFile: %s\n", filepath.c_str());
            FileEntry entry;
            Xp3Archive* archive;
            std::string directoryName;
            if (g_vfs.GetEntry(filepath, entry, archive)) {
                GLOG("File found in VFS: %s\n", filepath.c_str());
                auto hFile = g_vfs.OpenFile(entry, archive);
                if (hFile != INVALID_HANDLE_VALUE) {
                    *FileHandle = hFile;
                    IoStatusBlock->Status = FILE_OPENED;
                    IoStatusBlock->Information = 0;
                    GLOG("File opened successfully: %s. %p\n", filepath.c_str(), hFile);
                    return STATUS_SUCCESS;
                }
            } else if (g_vfs.IsRootDirectory(filepath)) {
                auto status = Real_NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
                if (status == STATUS_SUCCESS) {
                    g_vfs.AddExistedDirHandle(*FileHandle, "/");
                }
                return status;
            } else if (g_vfs.GetDirectoryName(filepath, directoryName)) {
                GLOG("Directory found in VFS: %s\n", filepath.c_str());
                auto status = Real_NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
                if (status == STATUS_SUCCESS) {
                    g_vfs.AddExistedDirHandle(*FileHandle, directoryName);
                    return status;
                }
                HANDLE hDir = g_vfs.OpenDirectory(directoryName);
                if (hDir == INVALID_HANDLE_VALUE) {
                    return STATUS_UNSUCCESSFUL;
                }
                *FileHandle = hDir;
                IoStatusBlock->Status = FILE_OPENED;
                IoStatusBlock->Information = 0;
                GLOG("Directory opened successfully: %s. %p\n", filepath.c_str(), hDir);
                return STATUS_SUCCESS;
            }
        }
    }
    return Real_NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtClose(
    IN HANDLE Handle
) {
    if (g_vfs.ContainsFile(Handle)) {
        GLOG("NtClose: %p\n", Handle);
        g_vfs.CloseFile(Handle);
        return STATUS_SUCCESS;
    }
    if (g_vfs.IsDirectoryHandle(Handle)) {
        GLOG("NtClose (directory handle): %p\n", Handle);
        g_vfs.CloseDirectory(Handle);
        return STATUS_SUCCESS;
    }
    if (g_vfs.IsSectionHandle(Handle)) {
        GLOG("NtClose (section handle): %p\n", Handle);
        g_vfs.RemoveSectionHandle(Handle);
    } else if (g_vfs.IsExistedDirHandle(Handle)) {
        GLOG("NtClose (existed dir handle): %p\n", Handle);
        g_vfs.RemoveExistedDirHandle(Handle);
    }
    return Real_NtClose(Handle);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtReadFile(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID Buffer,
    IN ULONG Length,
    IN PLARGE_INTEGER ByteOffset OPTIONAL,
    IN PULONG Key OPTIONAL
) {
    auto file = g_vfs.GetFile(FileHandle);
    if (file) {
        GLOG("NtReadFile: %p, Length: %lu, ByteOffset: %lld\n", FileHandle, Length, ByteOffset ? ByteOffset->QuadPart : -1);
        size_t bytesRead;
        if (ByteOffset) {
            int64_t offset = ByteOffset->QuadPart;
            size_t n = file->read_at((uint8_t*)Buffer, Length, offset);
            bytesRead = n;
            while (bytesRead < Length && n > 0) {
                n = file->read_at((uint8_t*)Buffer + bytesRead, Length - bytesRead, offset + bytesRead);
                bytesRead += n;
            }
        } else {
            size_t n = file->read((uint8_t*)Buffer, Length);
            bytesRead = n;
            while (bytesRead < Length && n > 0) {
                n = file->read((uint8_t*)Buffer + bytesRead, Length - bytesRead);
                bytesRead += n;
            }
        }
        GLOG("Bytes read: %zu\n", bytesRead);
        if (bytesRead == 0) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        } else {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = bytesRead;
        }
        auto cinfo = g_vfs.GetCompletionInfo(FileHandle);
        auto skipEvent = cinfo && (cinfo->Flags & FILE_SKIP_SET_EVENT_ON_HANDLE);
        if (Event && !skipEvent) {
            SetEvent(Event);
        }
        if (cinfo && !(cinfo->Flags & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
            Real_NtSetIoCompletion(
                cinfo->Port,
                cinfo->Key,
                ApcContext,
                STATUS_SUCCESS,
                bytesRead
            );
        }
        return STATUS_SUCCESS;
    } else if (g_vfs.IsDirectoryHandle(FileHandle)) {
        return STATUS_UNSUCCESSFUL;
    }
    return Real_NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryInformationFile(
    IN HANDLE FileHandle,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass
) {
    FileEntry entry;
    Xp3Archive* archive;
    if (g_vfs.GetFileInfo(FileHandle, entry, archive)) {
        GLOG("NtQueryInformationFile: %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
        if (FileInformationClass == FileStandardInformation) {
            if (Length < sizeof(FILE_STANDARD_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_STANDARD_INFORMATION* info = (FILE_STANDARD_INFORMATION*)FileInformation;
            info->AllocationSize.QuadPart = entry.original_size;
            info->EndOfFile.QuadPart = entry.original_size;
            info->NumberOfLinks = 1;
            info->DeletePending = FALSE;
            info->Directory = FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileBasicInformation) {
            if (Length < sizeof(FILE_BASIC_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_BASIC_INFORMATION* info = (FILE_BASIC_INFORMATION*)FileInformation;
            // Set creation, access, write, change time to current time for simplicity
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            info->CreationTime.LowPart = ft.dwLowDateTime;
            info->CreationTime.HighPart = ft.dwHighDateTime;
            info->LastAccessTime.LowPart = ft.dwLowDateTime;
            info->LastAccessTime.HighPart = ft.dwHighDateTime;
            info->LastWriteTime.LowPart = ft.dwLowDateTime;
            info->LastWriteTime.HighPart = ft.dwHighDateTime;
            info->ChangeTime.LowPart = ft.dwLowDateTime;
            info->ChangeTime.HighPart = ft.dwHighDateTime;
            info->FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileInternalInformation) {
            if (Length < sizeof(FILE_INTERNAL_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_INTERNAL_INFORMATION* info = (FILE_INTERNAL_INFORMATION*)FileInformation;
            info->IndexNumber.QuadPart = entry.adler32; // Use adler32 as a dummy index number
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_INTERNAL_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileEaInformation) {
            if (Length < sizeof(FILE_EA_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_EA_INFORMATION* info = (FILE_EA_INFORMATION*)FileInformation;
            info->EaSize = 0; // No extended attributes
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_EA_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileAccessInformation) {
            if (Length < sizeof(FILE_ACCESS_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_ACCESS_INFORMATION* info = (FILE_ACCESS_INFORMATION*)FileInformation;
            info->AccessFlags = READ_CONTROL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_ACCESS_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileNameInformation == FileInformationClass || FileNormalizedNameInformation == FileInformationClass) {
            if (Length < sizeof(FILE_NAME_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            std::wstring wFilename;
            if (!wchar_util::str_to_wstr(wFilename, g_vfs.GetRootPath(entry.filename), CP_UTF8)) {
                return STATUS_UNSUCCESSFUL;
            }
            size_t nameLenBytes = wFilename.length() * sizeof(WCHAR);
            size_t remainingSpaceBytes = Length - offsetof(FILE_NAME_INFORMATION, FileName);
            size_t bytesToCopy = min(nameLenBytes, remainingSpaceBytes);
            FILE_NAME_INFORMATION* info = (FILE_NAME_INFORMATION*)FileInformation;
            info->FileNameLength = (ULONG)nameLenBytes;
            memcpy(info->FileName, wFilename.c_str(), bytesToCopy);
            if (nameLenBytes > remainingSpaceBytes) {
                IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = offsetof(FILE_NAME_INFORMATION, FileName) + nameLenBytes;
                return STATUS_BUFFER_OVERFLOW;
            } else {
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = offsetof(FILE_NAME_INFORMATION, FileName) + nameLenBytes;
                return STATUS_SUCCESS;
            }
        } else if (FileInformationClass == FilePositionInformation) {
            if (Length < sizeof(FILE_POSITION_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_POSITION_INFORMATION* info = (FILE_POSITION_INFORMATION*)FileInformation;
            auto file = g_vfs.GetFile(FileHandle);
            if (file) {
                info->CurrentByteOffset.QuadPart = file->tell();
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
                return STATUS_SUCCESS;
            } else {
                return STATUS_UNSUCCESSFUL;
            }
        } else if (FileInformationClass == FileModeInformation) {
            if (Length < sizeof(FILE_MODE_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_MODE_INFORMATION* info = (FILE_MODE_INFORMATION*)FileInformation;
            info->Mode = 0; // No special mode
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_MODE_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileAlignmentInformation) {
            if (Length < sizeof(FILE_ALIGNMENT_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_ALIGNMENT_INFORMATION* info = (FILE_ALIGNMENT_INFORMATION*)FileInformation;
            info->AlignmentRequirement = FILE_BYTE_ALIGNMENT; // No special alignment requirement
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_ALIGNMENT_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileAllInformation) {
            if (Length < sizeof(FILE_ALL_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            std::wstring wFilename;
            if (!wchar_util::str_to_wstr(wFilename, g_vfs.GetRootPath(entry.filename), CP_UTF8)) {
                return STATUS_UNSUCCESSFUL;
            }
            FILE_ALL_INFORMATION* info = (FILE_ALL_INFORMATION*)FileInformation;
            // Fill basic information
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            info->BasicInformation.CreationTime.QuadPart = ft.dwLowDateTime | ((uint64_t)ft.dwHighDateTime << 32);
            info->BasicInformation.LastAccessTime.QuadPart = info->BasicInformation.CreationTime.QuadPart;
            info->BasicInformation.LastWriteTime.QuadPart = info->BasicInformation.CreationTime.QuadPart;
            info->BasicInformation.ChangeTime.QuadPart = info->BasicInformation.CreationTime.QuadPart;
            info->BasicInformation.FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
            // Fill standard information
            info->StandardInformation.AllocationSize.QuadPart = entry.original_size;
            info->StandardInformation.EndOfFile.QuadPart = entry.original_size;
            info->StandardInformation.NumberOfLinks = 1;
            info->StandardInformation.DeletePending = FALSE;
            info->StandardInformation.Directory = FALSE;
            // Fill internal information
            info->InternalInformation.IndexNumber.QuadPart = entry.adler32; // Use adler32 as a dummy index number
            // Fill EA information
            info->EaInformation.EaSize = 0; // No extended attributes
            // Fill access information
            info->AccessInformation.AccessFlags = READ_CONTROL;
            // Fill position information
            auto file = g_vfs.GetFile(FileHandle);
            if (file) {
                info->PositionInformation.CurrentByteOffset.QuadPart = file->tell();
            } else {
                info->PositionInformation.CurrentByteOffset.QuadPart = 0;
            }
            // Fill mode information
            info->ModeInformation.Mode = 0; // No special mode
            // Fill alignment information
            info->AlignmentInformation.AlignmentRequirement = FILE_BYTE_ALIGNMENT; // No special alignment requirement
            // Fill name information
            size_t nameLenBytes = wFilename.length() * sizeof(WCHAR);
            size_t remainingSpaceBytes = Length - offsetof(FILE_ALL_INFORMATION, NameInformation.FileName);
            size_t bytesToCopy = min(nameLenBytes, remainingSpaceBytes);
            info->NameInformation.FileNameLength = (ULONG)nameLenBytes;
            memcpy(info->NameInformation.FileName, wFilename.c_str(), bytesToCopy);
            if (nameLenBytes > remainingSpaceBytes) {
                IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = offsetof(FILE_ALL_INFORMATION, NameInformation.FileName) + nameLenBytes;
                return STATUS_BUFFER_OVERFLOW;
            } else {
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = offsetof(FILE_ALL_INFORMATION, NameInformation.FileName) + nameLenBytes;
                return STATUS_SUCCESS;
            }
        }
    } else if (g_vfs.IsDirectoryHandle(FileHandle)) {
        GLOG("NtQueryInformationFile (directory handle): %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
        if (FileInformationClass == FileNameInformation || FileInformationClass == FileNormalizedNameInformation) {
            if (Length < sizeof(FILE_NAME_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            auto hDir = (DirEntry*)FileHandle;
            std::string path = hDir->name.substr(1); // Remove leading slash
            if (path.back() == '/') {
                path.pop_back();
            }
            path = g_vfs.GetRootPath(path);
            std::wstring wDirectoryPath;
            if (!wchar_util::str_to_wstr(wDirectoryPath, path, CP_UTF8)) {
                return STATUS_UNSUCCESSFUL;
            }
            size_t nameLenBytes = wDirectoryPath.length() * sizeof(WCHAR);
            size_t remainingSpaceBytes = Length - offsetof(FILE_NAME_INFORMATION, FileName);
            size_t bytesToCopy = min(nameLenBytes, remainingSpaceBytes);
            FILE_NAME_INFORMATION* info = (FILE_NAME_INFORMATION*)FileInformation;
            info->FileNameLength = (ULONG)nameLenBytes;
            memcpy(info->FileName, wDirectoryPath.c_str(), bytesToCopy);
            if (nameLenBytes > remainingSpaceBytes) {
                IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = offsetof(FILE_NAME_INFORMATION, FileName) + nameLenBytes;
                return STATUS_BUFFER_OVERFLOW;
            } else {
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = offsetof(FILE_NAME_INFORMATION, FileName) + nameLenBytes;
                return STATUS_SUCCESS;
            }
        } else if (FileInformationClass == FileStandardInformation) {
            if (Length < sizeof(FILE_STANDARD_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_STANDARD_INFORMATION* info = (FILE_STANDARD_INFORMATION*)FileInformation;
            info->AllocationSize.QuadPart = 0;
            info->EndOfFile.QuadPart = 0;
            info->NumberOfLinks = 1;
            info->DeletePending = FALSE;
            info->Directory = TRUE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileBasicInformation) {
            if (Length < sizeof(FILE_BASIC_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_BASIC_INFORMATION* info = (FILE_BASIC_INFORMATION*)FileInformation;
            // Set creation, access, write, change time to current time for simplicity
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            info->CreationTime.QuadPart = ft.dwLowDateTime | ((uint64_t)ft.dwHighDateTime << 32);
            info->LastAccessTime.QuadPart = info->CreationTime.QuadPart;
            info->LastWriteTime.QuadPart = info->CreationTime.QuadPart;
            info->ChangeTime.QuadPart = info->CreationTime.QuadPart;
            info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileInternalInformation) {
            if (Length < sizeof(FILE_INTERNAL_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_INTERNAL_INFORMATION* info = (FILE_INTERNAL_INFORMATION*)FileInformation;
            info->IndexNumber.QuadPart = 0; // No real index number for directories
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_INTERNAL_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileEaInformation) {
            if (Length < sizeof(FILE_EA_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_EA_INFORMATION* info = (FILE_EA_INFORMATION*)FileInformation;
            info->EaSize = 0; // No extended attributes
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_EA_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileAccessInformation) {
            if (Length < sizeof(FILE_ACCESS_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_ACCESS_INFORMATION* info = (FILE_ACCESS_INFORMATION*)FileInformation;
            info->AccessFlags = READ_CONTROL | FILE_LIST_DIRECTORY;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_ACCESS_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FilePositionInformation) {
            if (Length < sizeof(FILE_POSITION_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_POSITION_INFORMATION* info = (FILE_POSITION_INFORMATION*)FileInformation;
            info->CurrentByteOffset.QuadPart = 0; // Directories don't have a byte offset
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileModeInformation) {
            if (Length < sizeof(FILE_MODE_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_MODE_INFORMATION* info = (FILE_MODE_INFORMATION*)FileInformation;
            info->Mode = 0; // No special mode
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_MODE_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileAlignmentInformation) {
            if (Length < sizeof(FILE_ALIGNMENT_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_ALIGNMENT_INFORMATION* info = (FILE_ALIGNMENT_INFORMATION*)FileInformation;
            info->AlignmentRequirement = FILE_BYTE_ALIGNMENT; // No special alignment requirement
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_ALIGNMENT_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileAllInformation) {
            if (Length < sizeof(FILE_ALL_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            std::wstring wDirectoryPath;
            auto hDir = (DirEntry*)FileHandle;
            std::string path = hDir->name.substr(1); // Remove leading slash
            if (path.back() == '/') {
                path.pop_back();
            }
            path = g_vfs.GetRootPath(path);
            if (!wchar_util::str_to_wstr(wDirectoryPath, path, CP_UTF8)) {
                return STATUS_UNSUCCESSFUL;
            }
            FILE_ALL_INFORMATION* info = (FILE_ALL_INFORMATION*)FileInformation;
            // Fill basic information
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            info->BasicInformation.CreationTime.QuadPart = ft.dwLowDateTime | ((uint64_t)ft.dwHighDateTime << 32);
            info->BasicInformation.LastAccessTime.QuadPart = info->BasicInformation.CreationTime.QuadPart;
            info->BasicInformation.LastWriteTime.QuadPart = info->BasicInformation.CreationTime.QuadPart;
            info->BasicInformation.ChangeTime.QuadPart = info->BasicInformation.CreationTime.QuadPart;
            info->BasicInformation.FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL;
            // Fill standard information
            info->StandardInformation.AllocationSize.QuadPart = 0;
            info->StandardInformation.EndOfFile.QuadPart = 0;
            info->StandardInformation.NumberOfLinks = 1;
            info->StandardInformation.DeletePending = FALSE;
            info->StandardInformation.Directory = TRUE;
            // Fill internal information
            info->InternalInformation.IndexNumber.QuadPart = 0; // No real index number for directories
            // Fill EA information
            info->EaInformation.EaSize = 0; // No extended attributes
            // Fill access information
            info->AccessInformation.AccessFlags = READ_CONTROL | FILE_LIST_DIRECTORY;
            // Fill position information
            info->PositionInformation.CurrentByteOffset.QuadPart = 0; // Directories don't have a byte offset
            // Fill mode information
            info->ModeInformation.Mode = 0; // No special mode
            // Fill alignment information
            info->AlignmentInformation.AlignmentRequirement = FILE_BYTE_ALIGNMENT; // No special alignment requirement
            // Fill name information
            size_t nameLenBytes = wDirectoryPath.length() * sizeof(WCHAR);
            size_t remainingSpaceBytes = Length - offsetof(FILE_ALL_INFORMATION, NameInformation.FileName);
            size_t bytesToCopy = min(nameLenBytes, remainingSpaceBytes);
            info->NameInformation.FileNameLength = (ULONG)nameLenBytes;
            memcpy(info->NameInformation.FileName, wDirectoryPath.c_str(), bytesToCopy);
            if (nameLenBytes > remainingSpaceBytes) {
                IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = offsetof(FILE_ALL_INFORMATION, NameInformation.FileName) + nameLenBytes;
                return STATUS_BUFFER_OVERFLOW;
            } else {
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = offsetof(FILE_ALL_INFORMATION, NameInformation.FileName) + nameLenBytes;
                return STATUS_SUCCESS;
            }
        }
    }
#if WINVFS_LOGGING
    if (g_vfs.InTrace(FileHandle)) {
        GLOG("NtQueryInformationFile (trace): %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
        auto result = Real_NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
        if (result == STATUS_SUCCESS && IoStatusBlock->Status == STATUS_SUCCESS) {
            if (FileInformationClass == FileNameInformation) {
                FILE_NAME_INFORMATION* info = (FILE_NAME_INFORMATION*)FileInformation;
                std::wstring wFilename(info->FileName, info->FileNameLength / sizeof(WCHAR));
                std::string filename;
                wchar_util::wstr_to_str(filename, wFilename, CP_UTF8);
                GLOG("Returned file name: %s\n", filename.c_str());
            }
        }
        return result;
    }
#endif
    return Real_NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryVolumeInformationFile(
    IN HANDLE FileHandle,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FsInformation,
    IN ULONG Length,
    IN FS_INFORMATION_CLASS FsInformationClass
) {
    FileEntry entry;
    Xp3Archive* archive;
    if (g_vfs.GetFileInfo(FileHandle, entry, archive) || g_vfs.IsDirectoryHandle(FileHandle)) {
        GLOG("NtQueryVolumeInformationFile: %p, FsInformationClass: %d\n", FileHandle, FsInformationClass);
        if (FsInformationClass == FileFsVolumeInformation) {
            if (Length < sizeof(FILE_FS_VOLUME_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_FS_VOLUME_INFORMATION* info = (FILE_FS_VOLUME_INFORMATION*)FsInformation;
            // Set volume creation time to current time for simplicity
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            info->VolumeCreationTime.LowPart = ft.dwLowDateTime;
            info->VolumeCreationTime.HighPart = ft.dwHighDateTime;
            info->VolumeSerialNumber = 0xDEADBEEF; // Dummy serial number
            wchar_t volumeLabel[] = L"VFS Volume";
            size_t labelLen = sizeof(volumeLabel) - sizeof(WCHAR);
            size_t remaingSpace = Length - offsetof(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
            size_t bytesToCopy = min(labelLen, remaingSpace);
            info->VolumeLabelLength = (ULONG)labelLen;
            memcpy(info->VolumeLabel, volumeLabel, bytesToCopy);
            if (labelLen > remaingSpace) {
                IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = offsetof(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + labelLen;
                return STATUS_BUFFER_OVERFLOW;
            } else {
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = offsetof(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + labelLen;
                return STATUS_SUCCESS;
            }
        } else if (FsInformationClass == FileFsDeviceInformation) {
            if (Length < sizeof(FILE_FS_DEVICE_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_FS_DEVICE_INFORMATION* info = (FILE_FS_DEVICE_INFORMATION*)FsInformation;
            info->DeviceType = FILE_DEVICE_DISK; // Report as disk device
            info->Characteristics = 0; // No special characteristics
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_FS_DEVICE_INFORMATION);
            return STATUS_SUCCESS;
        }
    }
#if WINVFS_LOGGING
    if (g_vfs.InTrace(FileHandle)) {
        GLOG("NtQueryVolumeInformationFile (trace): %p, FsInformationClass: %d\n", FileHandle, FsInformationClass);
        auto result = Real_NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass);
        return result;
    }
#endif
    return Real_NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryObject(
    IN HANDLE Handle OPTIONAL,
    IN OBJECT_INFORMATION_CLASS ObjectInformationClass,
    OUT PVOID ObjectInformation OPTIONAL,
    IN ULONG ObjectInformationLength,
    OUT PULONG ReturnLength OPTIONAL
) {
    if (Handle != INVALID_HANDLE_VALUE && ObjectInformation) {
        FileEntry entry;
        Xp3Archive* archive;
        if (g_vfs.GetFileInfo(Handle, entry, archive)) {
            GLOG("NtQueryObject: %p, ObjectInformationClass: %d\n", Handle, ObjectInformationClass);
            if (ObjectInformationClass == ObjectNameInformation) {
                std::string ntpath = g_vfs.GetNtPath(entry.filename);
                GLOG("Object name for handle %p: %s\n", Handle, ntpath.c_str());
                std::wstring wPath;
                if (!wchar_util::str_to_wstr(wPath, ntpath, CP_UTF8)) {
                    return STATUS_UNSUCCESSFUL;
                }
                ULONG requiredLength = sizeof(OBJECT_NAME_INFORMATION) + (ULONG)((wPath.length() + 1) * sizeof(wchar_t));
                if (ReturnLength) {
                    *ReturnLength = requiredLength;
                }
                if (ObjectInformationLength < requiredLength) {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }
                if (ObjectInformation) {
                    POBJECT_NAME_INFORMATION pNameInfo = (POBJECT_NAME_INFORMATION)ObjectInformation;
                    PWSTR destBuffer = (PWSTR)((PBYTE)ObjectInformation + sizeof(OBJECT_NAME_INFORMATION));
                    memcpy(destBuffer, wPath.c_str(), wPath.length() * sizeof(wchar_t));
                    destBuffer[wPath.length()] = L'\0';
                    pNameInfo->Name.Length = (USHORT)(wPath.length() * sizeof(wchar_t));
                    pNameInfo->Name.MaximumLength = (USHORT)((wPath.length() + 1) * sizeof(wchar_t));
                    pNameInfo->Name.Buffer = destBuffer;
                    return STATUS_SUCCESS;
                }
            }
        } else if (g_vfs.IsDirectoryHandle(Handle)) {
            GLOG("NtQueryObject (directory handle): %p, ObjectInformationClass: %d\n", Handle, ObjectInformationClass);
            if (ObjectInformationClass == ObjectNameInformation) {
                auto hDir = (DirEntry*)Handle;
                std::string path = hDir->name.substr(1); // Remove leading slash
                if (path.back() == '/') {
                    path.pop_back();
                }
                path = g_vfs.GetNtPath(path);
                std::wstring wPath;
                if (!wchar_util::str_to_wstr(wPath, path, CP_UTF8)) {
                    return STATUS_UNSUCCESSFUL;
                }
                ULONG requiredLength = sizeof(OBJECT_NAME_INFORMATION) + (ULONG)((wPath.length() + 1) * sizeof(wchar_t));
                if (ReturnLength) {
                    *ReturnLength = requiredLength;
                }
                if (ObjectInformationLength < requiredLength) {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }
                if (ObjectInformation) {
                    POBJECT_NAME_INFORMATION pNameInfo = (POBJECT_NAME_INFORMATION)ObjectInformation;
                    PWSTR destBuffer = (PWSTR)((PBYTE)ObjectInformation + sizeof(OBJECT_NAME_INFORMATION));
                    memcpy(destBuffer, wPath.c_str(), wPath.length() * sizeof(wchar_t));
                    destBuffer[wPath.length()] = L'\0';
                    pNameInfo->Name.Length = (USHORT)(wPath.length() * sizeof(wchar_t));
                    pNameInfo->Name.MaximumLength = (USHORT)((wPath.length() + 1) * sizeof(wchar_t));
                    pNameInfo->Name.Buffer = destBuffer;
                    return STATUS_SUCCESS;
                }
            }
        }
    }
#if WINVFS_LOGGING
    if (g_vfs.InTrace(Handle)) {
        GLOG("NtQueryObject (trace): %p, ObjectInformationClass: %d\n", Handle, ObjectInformationClass);
    }
#endif
    return Real_NtQueryObject(Handle, ObjectInformationClass, ObjectInformation, ObjectInformationLength, ReturnLength);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryAttributesFile(
    IN POBJECT_ATTRIBUTES ObjectAttributes,
    OUT PFILE_BASIC_INFORMATION FileInformation
) {
    if (ObjectAttributes->ObjectName && ObjectAttributes->ObjectName->Buffer) {
        std::wstring wFilepath(ObjectAttributes->ObjectName->Buffer, ObjectAttributes->ObjectName->Length / sizeof(WCHAR));
        std::string filepath;
        if (wchar_util::wstr_to_str(filepath, wFilepath, CP_UTF8)) {
            if (ObjectAttributes->RootDirectory) {
                auto hDir = ObjectAttributes->RootDirectory;
                std::string basePath = getFinalPath(hDir, FILE_NAME_NORMALIZED);
                filepath = basePath + "\\" + filepath;
            }
            GLOG("NtQueryAttributesFile: %s\n", filepath.c_str());
            FileEntry entry;
            Xp3Archive* archive;
            if (g_vfs.GetEntry(filepath, entry, archive)) {
                GLOG("File found in VFS: %s\n", filepath.c_str());
                // Set creation, access, write, change time to current time for simplicity
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                FileInformation->CreationTime.LowPart = ft.dwLowDateTime;
                FileInformation->CreationTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastAccessTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastAccessTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastWriteTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastWriteTime.HighPart = ft.dwHighDateTime;
                FileInformation->ChangeTime.LowPart = ft.dwLowDateTime;
                FileInformation->ChangeTime.HighPart = ft.dwHighDateTime;
                FileInformation->FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
                return STATUS_SUCCESS;
            } else if (g_vfs.HasDirectory(filepath)) {
                GLOG("Directory found in VFS: %s\n", filepath.c_str());
                auto re = Real_NtQueryAttributesFile(ObjectAttributes, FileInformation);
                if (re == STATUS_SUCCESS) {
                    return re;
                }
                // Set creation, access, write, change time to current time for simplicity
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                FileInformation->CreationTime.LowPart = ft.dwLowDateTime;
                FileInformation->CreationTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastAccessTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastAccessTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastWriteTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastWriteTime.HighPart = ft.dwHighDateTime;
                FileInformation->ChangeTime.LowPart = ft.dwLowDateTime;
                FileInformation->ChangeTime.HighPart = ft.dwHighDateTime;
                FileInformation->FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY;
                return STATUS_SUCCESS;
            }
        }
    }
    return Real_NtQueryAttributesFile(ObjectAttributes, FileInformation);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtSetInformationFile(
    IN HANDLE FileHandle,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass
) {
    FileEntry entry;
    Xp3Archive* archive;
    if (g_vfs.GetFileInfo(FileHandle, entry, archive)) {
        GLOG("NtSetInformationFile: %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
        if (FileInformationClass == FilePositionInformation) {
            if (Length < sizeof(FILE_POSITION_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            FILE_POSITION_INFORMATION* info = (FILE_POSITION_INFORMATION*)FileInformation;
            auto file = g_vfs.GetFile(FileHandle);
            if (file) {
                auto result = file->seek(info->CurrentByteOffset.QuadPart, SEEK_SET);
                if (result) {
                    IoStatusBlock->Status = STATUS_SUCCESS;
                    IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
                    return STATUS_SUCCESS;
                } else {
                    return STATUS_UNSUCCESSFUL;
                }
            } else {
                return STATUS_UNSUCCESSFUL;
            }
        } else if (FileInformationClass == FileCompletionInformation) {
            if (Length < sizeof(FILE_COMPLETION_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            auto completionInfo = (FILE_COMPLETION_INFORMATION*)FileInformation;
            auto now = g_vfs.GetCompletionInfo(FileHandle);
            if (now) {
                now->Key = completionInfo->Key;
                now->Port = completionInfo->Port;
            } else {
                auto newInfo = CompleteInfo { completionInfo->Port, completionInfo->Key, 0 };
                g_vfs.SetCompletionInfo(FileHandle, newInfo);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_COMPLETION_INFORMATION);
            return STATUS_SUCCESS;
        } else if (FileInformationClass == FileIoCompletionNotificationInformation) {
            if (Length < sizeof(FILE_IO_COMPLETION_NOTIFICATION_INFORMATION)) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            auto notificationInfo = (FILE_IO_COMPLETION_NOTIFICATION_INFORMATION*)FileInformation;
            auto now = g_vfs.GetCompletionInfo(FileHandle);
            if (now) {
                now->Flags = notificationInfo->Flags;
            } else {
                return STATUS_UNSUCCESSFUL;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(FILE_IO_COMPLETION_NOTIFICATION_INFORMATION);
            return STATUS_SUCCESS;
        }
    }
    return Real_NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtCreateSection(
    OUT PHANDLE SectionHandle,
    IN ACCESS_MASK DesiredAccess,
    IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
    IN PLARGE_INTEGER MaximumSize OPTIONAL,
    IN ULONG SectionPageProtection,
    IN ULONG AllocationAttributes,
    IN HANDLE FileHandle OPTIONAL
) {
    if (FileHandle != INVALID_HANDLE_VALUE) {
        FileEntry entry;
        Xp3Archive* archive;
        if (g_vfs.GetFileInfo(FileHandle, entry, archive)) {
            GLOG("NtCreateSection: %p\n", FileHandle);
            if (!(AllocationAttributes & SEC_IMAGE)) {
                auto file = g_vfs.GetFile(FileHandle);
                if (!file) {
                    return STATUS_UNSUCCESSFUL;
                }
                // For non-image sections. We don't use temporary files.
                LARGE_INTEGER sectionSize;
                if (MaximumSize) {
                    sectionSize = *MaximumSize;
                } else {
                    sectionSize.QuadPart = entry.original_size;
                }
                if (sectionSize.QuadPart == 0) {
                    sectionSize.QuadPart = entry.original_size;
                }
                if (sectionSize.QuadPart > entry.original_size) {
                    sectionSize.QuadPart = entry.original_size;
                }
                // Create a section in memory
                HANDLE hNewSection = NULL;
                NTSTATUS status = Real_NtCreateSection(&hNewSection, SECTION_ALL_ACCESS, ObjectAttributes, &sectionSize, PAGE_READWRITE, SEC_COMMIT, NULL);
                if (status != STATUS_SUCCESS) {
                    GLOG("Failed to create section: %08X\n", status);
                    return status;
                }
                PVOID pBase = NULL;
                SIZE_T viewSize = 0;
                status = Real_NtMapViewOfSection(hNewSection, (HANDLE)-1, &pBase, 0, 0, NULL, &viewSize, ViewShare, 0, PAGE_READWRITE);
                if (status != STATUS_SUCCESS) {
                    GLOG("Failed to map view of section: %08X\n", status);
                    Real_NtClose(hNewSection);
                    return status;
                }
                // Read file content into the section
                file->seek(0, SEEK_SET);
                bool ok = file->readall((uint8_t*)pBase, min(sectionSize.QuadPart, viewSize));
                if (!ok) {
                    Real_NtUnmapViewOfSection((HANDLE)-1, pBase);
                    Real_NtClose(hNewSection);
                    return STATUS_UNSUCCESSFUL;
                }
                GLOG("File content mapped to section successfully: %p, viewSize: %zu, fileSize: %llu, sectionSize: %lld, MaximumSize: %lld\n", pBase, viewSize, entry.original_size, sectionSize.QuadPart, MaximumSize ? MaximumSize->QuadPart : -1);
                status = Real_NtUnmapViewOfSection((HANDLE)-1, pBase);
                if (status != STATUS_SUCCESS) {
                    GLOG("Failed to unmap view of section: %08X\n", status);
                    Real_NtClose(hNewSection);
                    return status;
                }
                HANDLE hUserSection = NULL;
                HANDLE currentProcess = GetCurrentProcess();
                status = Real_NtDuplicateObject((HANDLE)-1, hNewSection, currentProcess, &hUserSection, DesiredAccess, 0, DUPLICATE_CLOSE_SOURCE);
                if (NT_SUCCESS(status)) {
                    *SectionHandle = hUserSection;
                } else {
                    GLOG("Failed to duplicate section handle: %08X\n", status);
                    Real_NtClose(hNewSection);
                }
                g_vfs.AddSectionHandle(hUserSection, std::pair(entry, archive));
                return status;
            }
        }
    }
    return Real_NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtMapViewOfSection(
    IN HANDLE SectionHandle,
    IN HANDLE ProcessHandle,
    IN OUT PVOID* BaseAddress,
    IN ULONG_PTR ZeroBits,
    IN SIZE_T CommitSize,
    IN OUT PLARGE_INTEGER SectionOffset OPTIONAL,
    IN OUT PSIZE_T ViewSize,
    IN SECTION_INHERIT InheritDisposition,
    IN ULONG AllocationType,
    IN ULONG Win32Protect
) {
    if (g_vfs.IsSectionHandle(SectionHandle)) {
        GLOG("NtMapViewOfSection: %p\n", SectionHandle);
        auto result = Real_NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType, Win32Protect);
        GLOG("Result: %08X, viewSize: %zu, SectionOffset: %lli\n", result, *ViewSize, SectionOffset ? SectionOffset->QuadPart : -1);
        if (!result) {
            FileEntry entry;
            Xp3Archive* archive;
            if (g_vfs.GetSectionInfo(SectionHandle, entry, archive)) {
                *ViewSize = min(*ViewSize, (SIZE_T)(entry.original_size - (SectionOffset ? SectionOffset->QuadPart : 0)));
                GLOG("Adjusted view size: %zu\n", *ViewSize);
            }
        }
        return result;
    }
    return Real_NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType, Win32Protect);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryFullAttributesFile(
    IN POBJECT_ATTRIBUTES ObjectAttributes,
    OUT PFILE_NETWORK_OPEN_INFORMATION FileInformation
) {
    if (ObjectAttributes->ObjectName && ObjectAttributes->ObjectName->Buffer) {
        std::wstring wFilepath(ObjectAttributes->ObjectName->Buffer, ObjectAttributes->ObjectName->Length / sizeof(WCHAR));
        std::string filepath;
        if (wchar_util::wstr_to_str(filepath, wFilepath, CP_UTF8)) {
            if (ObjectAttributes->RootDirectory) {
                auto hDir = ObjectAttributes->RootDirectory;
                std::string basePath = getFinalPath(hDir, FILE_NAME_NORMALIZED);
                filepath = basePath + "\\" + filepath;
            }
            GLOG("NtQueryFullAttributesFile: %s\n", filepath.c_str());
            FileEntry entry;
            Xp3Archive* archive;
            if (g_vfs.GetEntry(filepath, entry, archive)) {
                GLOG("File found in VFS: %s\n", filepath.c_str());
                // Set creation, access, write, change time to current time for simplicity
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                FileInformation->CreationTime.LowPart = ft.dwLowDateTime;
                FileInformation->CreationTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastAccessTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastAccessTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastWriteTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastWriteTime.HighPart = ft.dwHighDateTime;
                FileInformation->ChangeTime.LowPart = ft.dwLowDateTime;
                FileInformation->ChangeTime.HighPart = ft.dwHighDateTime;
                FileInformation->FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
                FileInformation->AllocationSize.QuadPart = entry.original_size;
                FileInformation->EndOfFile.QuadPart = entry.original_size;
                return STATUS_SUCCESS;
            } else if (g_vfs.HasDirectory(filepath)) {
                GLOG("Directory found in VFS: %s\n", filepath.c_str());
                auto re = Real_NtQueryFullAttributesFile(ObjectAttributes, FileInformation);
                if (re == STATUS_SUCCESS) {
                    return re;
                }
                // Set creation, access, write, change time to current time for simplicity
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                FileInformation->CreationTime.LowPart = ft.dwLowDateTime;
                FileInformation->CreationTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastAccessTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastAccessTime.HighPart = ft.dwHighDateTime;
                FileInformation->LastWriteTime.LowPart = ft.dwLowDateTime;
                FileInformation->LastWriteTime.HighPart = ft.dwHighDateTime;
                FileInformation->ChangeTime.LowPart = ft.dwLowDateTime;
                FileInformation->ChangeTime.HighPart = ft.dwHighDateTime;
                FileInformation->FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY;
                return STATUS_SUCCESS;
            }
        }
    }
    return Real_NtQueryFullAttributesFile(ObjectAttributes, FileInformation);
}

#define ENTRY_BLOCK_SIZE 64

template <typename T>
NTSTATUS CollectEntries(
    IN HANDLE FileHandle,
    IN FILE_INFORMATION_CLASS InformationClass,
    IN PUNICODE_STRING FilePath,
    DirEntriesCache<T>& entries
) {
    BOOLEAN RestartScan = TRUE;
    IO_STATUS_BLOCK IoStatusBlock;
    ZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    size_t BufferSize = sizeof(T) * ENTRY_BLOCK_SIZE;
    PVOID Buffer = malloc(BufferSize);
    if (!Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NTSTATUS status;
    do {
        status = Real_NtQueryDirectoryFile(FileHandle, NULL, NULL, NULL, &IoStatusBlock, Buffer, (ULONG)BufferSize, InformationClass, FALSE, FilePath, RestartScan);
        if (status == STATUS_SUCCESS) {
            size_t offset = 0;
            while (offset < IoStatusBlock.Information) {
                T* entry = (T*)((BYTE*)Buffer + offset);
                ULONG nextOffset = entry->NextEntryOffset;
                size_t length = nextOffset == 0 ? (IoStatusBlock.Information - offset) : nextOffset;
                T* entryCopy = (T*)malloc(length);
                if (!entryCopy) {
                    free(Buffer);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                memcpy(entryCopy, entry, length);
                entryCopy->NextEntryOffset = (ULONG)length; // Set NextEntryOffset to the actual size of this entry block
                entries.push_back(entryCopy);
                if (nextOffset == 0) {
                    break;
                }
                offset += nextOffset;
            }
            RestartScan = FALSE;
        } else if (status == STATUS_BUFFER_OVERFLOW) {
            size_t newBufferSize = BufferSize * 2;
            void* newBuffer = realloc(Buffer, newBufferSize);
            GLOG("Buffer overflow, resizing buffer from %zu to %zu bytes\n", BufferSize, newBufferSize);
            if (!newBuffer) {
                free(Buffer);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Buffer = newBuffer;
            BufferSize = newBufferSize;
            continue;
        } else if (status != STATUS_NO_MORE_FILES && status != STATUS_NO_SUCH_FILE) {
            free(Buffer);
            return status;
        }
    } while (status == STATUS_SUCCESS || status == STATUS_BUFFER_OVERFLOW);
    free(Buffer);
    return STATUS_SUCCESS;
}

template <typename T>
NTSTATUS CollectEntriesEx(
    IN HANDLE FileHandle,
    IN FILE_INFORMATION_CLASS InformationClass,
    IN PUNICODE_STRING FilePath,
    IN ULONG QueryFlags,
    DirEntriesCache<T>& entries
) {
    ULONG Flags = QueryFlags;
    // Remove SL_RESTART_SCAN flag and SL_RETURN_SINGLE_ENTRY flag for internal use, we will handle them ourselves
    if (Flags & SL_RESTART_SCAN) {
        Flags &= ~SL_RESTART_SCAN;
    }
    if (Flags & SL_RETURN_SINGLE_ENTRY) {
        Flags &= ~SL_RETURN_SINGLE_ENTRY;
    }
    ULONG RestartScan = SL_RESTART_SCAN;
    IO_STATUS_BLOCK IoStatusBlock;
    ZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    size_t BufferSize = sizeof(T) * ENTRY_BLOCK_SIZE;
    PVOID Buffer = malloc(BufferSize);
    if (!Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NTSTATUS status;
    do {
        status = Real_NtQueryDirectoryFileEx(FileHandle, NULL, NULL, NULL, &IoStatusBlock, Buffer, (ULONG)BufferSize, InformationClass, Flags | RestartScan, FilePath);
        if (status == STATUS_SUCCESS) {
            size_t offset = 0;
            while (offset < IoStatusBlock.Information) {
                T* entry = (T*)((BYTE*)Buffer + offset);
                ULONG nextOffset = entry->NextEntryOffset;
                size_t length = nextOffset == 0 ? (IoStatusBlock.Information - offset) : nextOffset;
                T* entryCopy = (T*)malloc(length);
                if (!entryCopy) {
                    free(Buffer);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                memcpy(entryCopy, entry, length);
                entryCopy->NextEntryOffset = (ULONG)length; // Set NextEntryOffset to the actual size of this entry block
                entries.push_back(entryCopy);
                if (nextOffset == 0) {
                    break;
                }
                offset += nextOffset;
            }
            RestartScan = 0;
        } else if (status == STATUS_BUFFER_OVERFLOW) {
            size_t newBufferSize = BufferSize * 2;
            void* newBuffer = realloc(Buffer, newBufferSize);
            GLOG("Buffer overflow, resizing buffer from %zu to %zu bytes\n", BufferSize, newBufferSize);
            if (!newBuffer) {
                free(Buffer);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Buffer = newBuffer;
            BufferSize = newBufferSize;
            continue;
        } else if (status != STATUS_NO_MORE_FILES && status != STATUS_NO_SUCH_FILE) {
            free(Buffer);
            return status;
        }
    } while (status == STATUS_SUCCESS || status == STATUS_BUFFER_OVERFLOW);
    free(Buffer);
    return STATUS_SUCCESS;
}

FILE_DIRECTORY_INFORMATION* GenFileDirectoryInformation(std::string& path, std::string entry) {
    std::wstring wEntry;
    if (!wchar_util::str_to_wstr(wEntry, entry, CP_UTF8)) {
        return nullptr;
    }
    std::string fullPath = path + entry;
    fullPath = fullPath.substr(1);
    FileEntry fileEntry;
    bool isDir = false;
    bool found = g_vfs.GetFileEntry(fullPath, fileEntry);
    if (wEntry.back() == '/') {
        wEntry.pop_back();
        isDir = true;
    }
    ULONG fileNameLength = (ULONG)(wEntry.length() * sizeof(WCHAR));
    size_t entrySize = sizeof(FILE_DIRECTORY_INFORMATION) + fileNameLength;
    FILE_DIRECTORY_INFORMATION* info = (FILE_DIRECTORY_INFORMATION*)malloc(entrySize);
    if (!info) {
        return nullptr;
    }
    ZeroMemory(info, entrySize);
    info->NextEntryOffset = entrySize;
    info->FileIndex = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    info->CreationTime.LowPart = ft.dwLowDateTime;
    info->CreationTime.HighPart = ft.dwHighDateTime;
    info->LastAccessTime.LowPart = ft.dwLowDateTime;
    info->LastAccessTime.HighPart = ft.dwHighDateTime;
    info->LastWriteTime.LowPart = ft.dwLowDateTime;
    info->LastWriteTime.HighPart = ft.dwHighDateTime;
    info->ChangeTime.LowPart = ft.dwLowDateTime;
    info->ChangeTime.HighPart = ft.dwHighDateTime;
    if (found) {
        info->EndOfFile.QuadPart = fileEntry.original_size;
        info->AllocationSize.QuadPart = fileEntry.original_size;
    }
    info->FileNameLength = fileNameLength;
    info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (isDir) {
        info->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        info->FileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    memcpy(info->FileName, wEntry.c_str(), fileNameLength);
    info->FileName[info->FileNameLength / sizeof(WCHAR)] = L'\0';
    return info;
}

FILE_FULL_DIR_INFORMATION* GenFileFullDirInformation(std::string& path, std::string entry) {
    std::wstring wEntry;
    if (!wchar_util::str_to_wstr(wEntry, entry, CP_UTF8)) {
        return nullptr;
    }
    std::string fullPath = path + entry;
    fullPath = fullPath.substr(1);
    FileEntry fileEntry;
    bool isDir = false;
    bool found = g_vfs.GetFileEntry(fullPath, fileEntry);
    if (wEntry.back() == '/') {
        wEntry.pop_back();
        isDir = true;
    }
    ULONG fileNameLength = (ULONG)(wEntry.length() * sizeof(WCHAR));
    size_t entrySize = sizeof(FILE_FULL_DIR_INFORMATION) + fileNameLength;
    FILE_FULL_DIR_INFORMATION* info = (FILE_FULL_DIR_INFORMATION*)malloc(entrySize);
    if (!info) {
        return nullptr;
    }
    ZeroMemory(info, entrySize);
    info->NextEntryOffset = entrySize;
    info->FileIndex = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    info->CreationTime.LowPart = ft.dwLowDateTime;
    info->CreationTime.HighPart = ft.dwHighDateTime;
    info->LastAccessTime.LowPart = ft.dwLowDateTime;
    info->LastAccessTime.HighPart = ft.dwHighDateTime;
    info->LastWriteTime.LowPart = ft.dwLowDateTime;
    info->LastWriteTime.HighPart = ft.dwHighDateTime;
    info->ChangeTime.LowPart = ft.dwLowDateTime;
    info->ChangeTime.HighPart = ft.dwHighDateTime;
    if (found) {
        info->EndOfFile.QuadPart = fileEntry.original_size;
        info->AllocationSize.QuadPart = fileEntry.original_size;
    }
    info->FileNameLength = fileNameLength;
    info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (isDir) {
        info->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        info->FileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    memcpy(info->FileName, wEntry.c_str(), fileNameLength);
    info->FileName[info->FileNameLength / sizeof(WCHAR)] = L'\0';
    return info;
}

FILE_BOTH_DIR_INFORMATION* GenFileBothDirInformation(std::string& path, std::string entry) {
    std::wstring wEntry;
    if (!wchar_util::str_to_wstr(wEntry, entry, CP_UTF8)) {
        return nullptr;
    }
    std::string fullPath = path + entry;
    fullPath = fullPath.substr(1);
    FileEntry fileEntry;
    bool isDir = false;
    bool found = g_vfs.GetFileEntry(fullPath, fileEntry);
    if (wEntry.back() == '/') {
        wEntry.pop_back();
        isDir = true;
    }
    ULONG fileNameLength = (ULONG)(wEntry.length() * sizeof(WCHAR));
    size_t entrySize = sizeof(FILE_BOTH_DIR_INFORMATION) + fileNameLength;
    FILE_BOTH_DIR_INFORMATION* info = (FILE_BOTH_DIR_INFORMATION*)malloc(entrySize);
    if (!info) {
        return nullptr;
    }
    ZeroMemory(info, entrySize);
    info->NextEntryOffset = entrySize;
    info->FileIndex = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    info->CreationTime.LowPart = ft.dwLowDateTime;
    info->CreationTime.HighPart = ft.dwHighDateTime;
    info->LastAccessTime.LowPart = ft.dwLowDateTime;
    info->LastAccessTime.HighPart = ft.dwHighDateTime;
    info->LastWriteTime.LowPart = ft.dwLowDateTime;
    info->LastWriteTime.HighPart = ft.dwHighDateTime;
    info->ChangeTime.LowPart = ft.dwLowDateTime;
    info->ChangeTime.HighPart = ft.dwHighDateTime;
    if (found) {
        info->EndOfFile.QuadPart = fileEntry.original_size;
        info->AllocationSize.QuadPart = fileEntry.original_size;
    }
    info->FileNameLength = fileNameLength;
    info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (isDir) {
        info->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        info->FileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    info->EaSize = 0;
    info->ShortNameLength = 0;
    memcpy(info->FileName, wEntry.c_str(), fileNameLength);
    info->FileName[info->FileNameLength / sizeof(WCHAR)] = L'\0';
    return info;
}

FILE_NAMES_INFORMATION* GenFileNamesInformation(std::string& path, std::string entry) {
    std::wstring wEntry;
    if (!wchar_util::str_to_wstr(wEntry, entry, CP_UTF8)) {
        return nullptr;
    }
    std::string fullPath = path + entry;
    fullPath = fullPath.substr(1);
    FileEntry fileEntry;
    bool isDir = false;
    bool found = g_vfs.GetFileEntry(fullPath, fileEntry);
    if (wEntry.back() == '/') {
        wEntry.pop_back();
        isDir = true;
    }
    ULONG fileNameLength = (ULONG)(wEntry.length() * sizeof(WCHAR));
    size_t entrySize = sizeof(FILE_NAMES_INFORMATION) + fileNameLength;
    FILE_NAMES_INFORMATION* info = (FILE_NAMES_INFORMATION*)malloc(entrySize);
    if (!info) {
        return nullptr;
    }
    ZeroMemory(info, entrySize);
    info->NextEntryOffset = entrySize;
    info->FileIndex = 0;
    memcpy(info->FileName, wEntry.c_str(), fileNameLength);
    info->FileName[info->FileNameLength / sizeof(WCHAR)] = L'\0';
    return info;
}

FILE_ID_BOTH_DIR_INFORMATION* GenFileIdBothDirInformation(std::string& path, std::string entry) {
    std::wstring wEntry;
    if (!wchar_util::str_to_wstr(wEntry, entry, CP_UTF8)) {
        return nullptr;
    }
    std::string fullPath = path + entry;
    fullPath = fullPath.substr(1);
    FileEntry fileEntry;
    bool isDir = false;
    bool found = g_vfs.GetFileEntry(fullPath, fileEntry);
    if (wEntry.back() == '/') {
        wEntry.pop_back();
        isDir = true;
    }
    ULONG fileNameLength = (ULONG)(wEntry.length() * sizeof(WCHAR));
    size_t entrySize = sizeof(FILE_ID_BOTH_DIR_INFORMATION) + fileNameLength;
    FILE_ID_BOTH_DIR_INFORMATION* info = (FILE_ID_BOTH_DIR_INFORMATION*)malloc(entrySize);
    if (!info) {
        return nullptr;
    }
    ZeroMemory(info, entrySize);
    info->NextEntryOffset = entrySize;
    info->FileIndex = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    info->CreationTime.LowPart = ft.dwLowDateTime;
    info->CreationTime.HighPart = ft.dwHighDateTime;
    info->LastAccessTime.LowPart = ft.dwLowDateTime;
    info->LastAccessTime.HighPart = ft.dwHighDateTime;
    info->LastWriteTime.LowPart = ft.dwLowDateTime;
    info->LastWriteTime.HighPart = ft.dwHighDateTime;
    info->ChangeTime.LowPart = ft.dwLowDateTime;
    info->ChangeTime.HighPart = ft.dwHighDateTime;
    if (found) {
        info->EndOfFile.QuadPart = fileEntry.original_size;
        info->AllocationSize.QuadPart = fileEntry.original_size;
        info->FileId.QuadPart = fileEntry.adler32;
    }
    info->FileNameLength = fileNameLength;
    info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (isDir) {
        info->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        info->FileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    memcpy(info->FileName, wEntry.c_str(), fileNameLength);
    info->FileName[info->FileNameLength / sizeof(WCHAR)] = L'\0';
    return info;
}

FILE_ID_FULL_DIR_INFORMATION* GenFileIdFullDirInformation(std::string& path, std::string entry) {
    std::wstring wEntry;
    if (!wchar_util::str_to_wstr(wEntry, entry, CP_UTF8)) {
        return nullptr;
    }
    std::string fullPath = path + entry;
    fullPath = fullPath.substr(1);
    FileEntry fileEntry;
    bool isDir = false;
    bool found = g_vfs.GetFileEntry(fullPath, fileEntry);
    if (wEntry.back() == '/') {
        wEntry.pop_back();
        isDir = true;
    }
    ULONG fileNameLength = (ULONG)(wEntry.length() * sizeof(WCHAR));
    size_t entrySize = sizeof(FILE_ID_FULL_DIR_INFORMATION) + fileNameLength;
    FILE_ID_FULL_DIR_INFORMATION* info = (FILE_ID_FULL_DIR_INFORMATION*)malloc(entrySize);
    if (!info) {
        return nullptr;
    }
    ZeroMemory(info, entrySize);
    info->NextEntryOffset = entrySize;
    info->FileIndex = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    info->CreationTime.LowPart = ft.dwLowDateTime;
    info->CreationTime.HighPart = ft.dwHighDateTime;
    info->LastAccessTime.LowPart = ft.dwLowDateTime;
    info->LastAccessTime.HighPart = ft.dwHighDateTime;
    info->LastWriteTime.LowPart = ft.dwLowDateTime;
    info->LastWriteTime.HighPart = ft.dwHighDateTime;
    info->ChangeTime.LowPart = ft.dwLowDateTime;
    info->ChangeTime.HighPart = ft.dwHighDateTime;
    if (found) {
        info->EndOfFile.QuadPart = fileEntry.original_size;
        info->AllocationSize.QuadPart = fileEntry.original_size;
        info->FileId.QuadPart = fileEntry.adler32;
    }
    info->FileNameLength = fileNameLength;
    info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (isDir) {
        info->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        info->FileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    memcpy(info->FileName, wEntry.c_str(), fileNameLength);
    info->FileName[info->FileNameLength / sizeof(WCHAR)] = L'\0';
    return info;
}

template <typename T>
NTSTATUS GenerateEntries(
    DirEntriesCache<T>& entries,
    std::string path,
    IN PUNICODE_STRING FileName,
    std::function<T*(std::string&,std::string)> callback,
    bool replace = false
) {
    auto& flist = g_vfs.GetDirectoryEntries(path);
    std::wstring wFileName;
    UNICODE_STRING pattern;
    if (FileName) {
        wFileName.assign(FileName->Buffer, FileName->Length / sizeof(WCHAR));
        // Convert to upper case
        for (auto& ch : wFileName) {
            ch = towupper(ch);
        }
        pattern.Length = (USHORT)wFileName.length() * sizeof(WCHAR);
        pattern.MaximumLength = pattern.Length + sizeof(WCHAR);
        pattern.Buffer = (PWSTR)wFileName.c_str();
    }
    for (auto& entry : flist) {
        if (FileName) {
            std::wstring wEntry;
            if (!wchar_util::str_to_wstr(wEntry, entry, CP_UTF8)) {
                return STATUS_UNSUCCESSFUL;
            }
            if (wEntry.back() == L'/') {
                wEntry.pop_back();
            }
            UNICODE_STRING s;
            s.Length = (USHORT)(wEntry.length() * sizeof(WCHAR));
            s.MaximumLength = s.Length + sizeof(WCHAR);
            s.Buffer = (PWSTR)wEntry.c_str();
            if (!Real_RtlIsNameInExpression(&pattern, &s, TRUE, NULL)) {
                continue;
            }
        }
        T* info = callback(path, entry);
        if (!info) {
            return STATUS_UNSUCCESSFUL;
        }
        entries.push_back(info, replace);
    }
    return STATUS_SUCCESS;
}

template <typename T>
NTSTATUS NtQueryDirectoryFileInternel(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass,
    BOOLEAN ReturnSingleEntry,
    BOOLEAN RestartScan,
    PUNICODE_STRING FileName,
    std::function<T*(std::string&,std::string)> callback
) {
    if (Length < sizeof(T)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    auto cache = g_vfs.GetDirEntriesCache<T>(FileHandle, FileInformationClass);
    if (cache && RestartScan) {
        g_vfs.RemoveDirEntriesCache<T>(FileHandle, FileInformationClass);
        cache = nullptr;
    }
    if (!cache) {
        cache = new DirEntriesCache<T>();
        bool isExistedHandle = g_vfs.IsExistedDirHandle(FileHandle);
        std::string path;
        NTSTATUS status;
        if (isExistedHandle) {
            status = CollectEntries<T>(FileHandle, FileInformationClass, FileName, *cache);
            if (status != STATUS_SUCCESS) {
                delete cache;
                return status;
            }
            path = g_vfs.GetExistedDirHandlePath(FileHandle);
        } else {
            auto hDir = (DirEntry*)FileHandle;
            path = hDir->name;
        }
        status = GenerateEntries<T>(*cache, path, FileName, callback, isExistedHandle);
        if (status != STATUS_SUCCESS) {
            delete cache;
            return status;
        }
        if (cache->empty()) {
            delete cache;
            IoStatusBlock->Status = STATUS_NO_SUCH_FILE;
            IoStatusBlock->Information = 0;
            return STATUS_NO_SUCH_FILE;
        }
        g_vfs.AddDirEntriesCache<T>(FileHandle, FileInformationClass, cache);
    }
    auto entry = cache->peek_one();
    if (!entry) {
        g_vfs.RemoveDirEntriesCache<T>(FileHandle, FileInformationClass);
        IoStatusBlock->Status = STATUS_NO_MORE_FILES;
        IoStatusBlock->Information = 0;
        return STATUS_NO_MORE_FILES;
    }
    ULONG offset = 0;
    ULONG preOffset = 0;
    do {
        ULONG entrySize = entry->NextEntryOffset;
        if (offset + entrySize > Length) {
            if (offset == 0) {
                // The buffer is too small to hold even a single entry
                IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = 0;
                return STATUS_BUFFER_OVERFLOW;
            } else {
                // Return the entries that can fit in the buffer
                break;
            }
        }
        preOffset = offset;
        offset += entrySize;
        memcpy((BYTE*)FileInformation + preOffset, entry, entrySize);
        cache->inc_one();
        if (ReturnSingleEntry) {
            break;
        }
        entry = cache->peek_one();
    } while (entry);
    auto status = STATUS_SUCCESS;
    if (offset > 0) {
        ULONG* dest = (ULONG*)((BYTE*)FileInformation + preOffset);
        *dest = 0;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = offset;
    return STATUS_SUCCESS;
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryDirectoryFile(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass,
    IN BOOLEAN ReturnSingleEntry,
    IN PUNICODE_STRING FileName OPTIONAL,
    IN BOOLEAN RestartScan
) {
    if (g_vfs.IsExistedDirHandle(FileHandle) || g_vfs.IsDirectoryHandle(FileHandle)) {
        GLOG("NtQueryDirectoryFile: %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
        if (FileInformationClass == FileBothDirectoryInformation) {
            return NtQueryDirectoryFileInternel<FILE_BOTH_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, RestartScan, FileName, GenFileBothDirInformation);
        } else if (FileInformationClass == FileDirectoryInformation) {
            return NtQueryDirectoryFileInternel<FILE_DIRECTORY_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, RestartScan, FileName, GenFileDirectoryInformation);
        } else if (FileInformationClass == FileFullDirectoryInformation) {
            return NtQueryDirectoryFileInternel<FILE_FULL_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, RestartScan, FileName, GenFileFullDirInformation);
        } else if (FileInformationClass == FileNamesInformation) {
            return NtQueryDirectoryFileInternel<FILE_NAMES_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, RestartScan, FileName, GenFileNamesInformation);
        } else if (FileInformationClass == FileIdBothDirectoryInformation) {
            return NtQueryDirectoryFileInternel<FILE_ID_BOTH_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, RestartScan, FileName, GenFileIdBothDirInformation);
        } else if (FileInformationClass == FileIdFullDirectoryInformation) {
            return NtQueryDirectoryFileInternel<FILE_ID_FULL_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, RestartScan, FileName, GenFileIdFullDirInformation);
        }
    }
    return Real_NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan);
}

template <typename T>
NTSTATUS NtQueryDirectoryFileExInternel(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass,
    ULONG QueryFlags,
    PUNICODE_STRING FileName,
    std::function<T*(std::string&,std::string)> callback
) {
    if (Length < sizeof(T)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    auto cache = g_vfs.GetDirEntriesCache<T>(FileHandle, FileInformationClass);
    if (cache && (QueryFlags & SL_RESTART_SCAN)) {
        g_vfs.RemoveDirEntriesCache<T>(FileHandle, FileInformationClass);
        cache = nullptr;
    }
    if (!cache) {
        cache = new DirEntriesCache<T>();
        bool isExistedHandle = g_vfs.IsExistedDirHandle(FileHandle);
        std::string path;
        NTSTATUS status;
        if (isExistedHandle) {
            status = CollectEntriesEx<T>(FileHandle, FileInformationClass, FileName, QueryFlags, *cache);
            if (status != STATUS_SUCCESS) {
                delete cache;
                return status;
            }
            path = g_vfs.GetExistedDirHandlePath(FileHandle);
        } else {
            auto hDir = (DirEntry*)FileHandle;
            path = hDir->name;
        }
        status = GenerateEntries<T>(*cache, path, FileName, callback, isExistedHandle);
        if (status != STATUS_SUCCESS) {
            delete cache;
            return status;
        }
        if (cache->empty()) {
            delete cache;
            IoStatusBlock->Status = STATUS_NO_SUCH_FILE;
            IoStatusBlock->Information = 0;
            return STATUS_NO_SUCH_FILE;
        }
        g_vfs.AddDirEntriesCache<T>(FileHandle, FileInformationClass, cache);
    }
    bool SingleEntry = (QueryFlags & SL_RETURN_SINGLE_ENTRY) != 0;
    auto entry = cache->peek_one();
    if (!entry) {
        g_vfs.RemoveDirEntriesCache<T>(FileHandle, FileInformationClass);
        IoStatusBlock->Status = STATUS_NO_MORE_FILES;
        IoStatusBlock->Information = 0;
        return STATUS_NO_MORE_FILES;
    }
    ULONG offset = 0;
    ULONG preOffset = 0;
    do {
        ULONG entrySize = entry->NextEntryOffset;
        if (offset + entrySize > Length) {
            if (offset == 0) {
                // The buffer is too small to hold even a single entry
                IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
                IoStatusBlock->Information = 0;
                return STATUS_BUFFER_OVERFLOW;
            } else {
                // Return the entries that can fit in the buffer
                break;
            }
        }
        preOffset = offset;
        offset += entrySize;
        memcpy((BYTE*)FileInformation + preOffset, entry, entrySize);
        cache->inc_one();
        if (SingleEntry) {
            break;
        }
        entry = cache->peek_one();
    } while (entry);
    auto status = STATUS_SUCCESS;
    if (offset > 0) {
        ULONG* dest = (ULONG*)((BYTE*)FileInformation + preOffset);
        *dest = 0;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = offset;
    return STATUS_SUCCESS;
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryDirectoryFileEx(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass,
    IN ULONG QueryFlags,
    IN PUNICODE_STRING FileName OPTIONAL
) {
    if (g_vfs.IsExistedDirHandle(FileHandle) || g_vfs.IsDirectoryHandle(FileHandle)) {
        GLOG("NtQueryDirectoryFileEx: %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
        if (FileInformationClass == FileBothDirectoryInformation) {
            return NtQueryDirectoryFileExInternel<FILE_BOTH_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName, GenFileBothDirInformation);
        } else if (FileInformationClass == FileDirectoryInformation) {
            return NtQueryDirectoryFileExInternel<FILE_DIRECTORY_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName, GenFileDirectoryInformation);
        } else if (FileInformationClass == FileFullDirectoryInformation) {
            return NtQueryDirectoryFileExInternel<FILE_FULL_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName, GenFileFullDirInformation);
        } else if (FileInformationClass == FileNamesInformation) {
            return NtQueryDirectoryFileExInternel<FILE_NAMES_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName, GenFileNamesInformation);
        } else if (FileInformationClass == FileIdBothDirectoryInformation) {
            return NtQueryDirectoryFileExInternel<FILE_ID_BOTH_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName, GenFileIdBothDirInformation);
        } else if (FileInformationClass == FileIdFullDirectoryInformation) {
            return NtQueryDirectoryFileExInternel<FILE_ID_FULL_DIR_INFORMATION>(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName, GenFileIdFullDirInformation);
        }
    }
    return Real_NtQueryDirectoryFileEx(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtWriteFile(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN PVOID Buffer,
    IN ULONG Length,
    IN PLARGE_INTEGER ByteOffset OPTIONAL,
    IN PULONG Key OPTIONAL
) {
    if (g_vfs.GetFile(FileHandle) || g_vfs.IsDirectoryHandle(FileHandle)) {
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }
    return Real_NtWriteFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);
}

VFS::VFS() {
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring path = exePath;
    std::string pathStr;
    if (!wchar_util::wstr_to_str(pathStr, path, CP_UTF8)) {
        char buf[MAX_PATH];
        GetModuleFileNameA(NULL, buf, MAX_PATH);
        pathStr = buf;
    }
    base_path = fileop::dirname(pathStr);
    base_path = str_util::str_replace(base_path, "/", "\\");
    base_path += "\\";
    std::wstring wBasePath;
    if (wchar_util::str_to_wstr(wBasePath, base_path, CP_UTF8)) {
        auto hDir = CreateFileW(wBasePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hDir != INVALID_HANDLE_VALUE) {
            dos_path = getFinalPath(hDir, FILE_NAME_NORMALIZED);
            guid_path = getFinalPath(hDir, VOLUME_NAME_GUID);
            nt_path = getFinalPath(hDir, VOLUME_NAME_NT);
            CloseHandle(hDir);
        }
    }
    if (!dos_path.empty()) {
        dos_path += "\\";
        // replace \\?\ with \??\ 
        dos_system_path = "\\??\\" + dos_path.substr(4);
    }
    if (!guid_path.empty()) {
        guid_path += "\\";
    }
    if (!nt_path.empty()) {
        nt_path += "\\";
    }
}

void CleanupCache(std::pair<HANDLE, FILE_INFORMATION_CLASS> info, void*& cache) {
    if (info.second == FileBothDirectoryInformation) {
        delete (DirEntriesCache<FILE_BOTH_DIR_INFORMATION>*)cache;
    } else if (info.second == FileDirectoryInformation) {
        delete (DirEntriesCache<FILE_DIRECTORY_INFORMATION>*)cache;
    } else if (info.second == FileFullDirectoryInformation) {
        delete (DirEntriesCache<FILE_FULL_DIR_INFORMATION>*)cache;
    } else if (info.second == FileNamesInformation) {
        delete (DirEntriesCache<FILE_NAMES_INFORMATION>*)cache;
    } else if (info.second == FileIdBothDirectoryInformation) {
        delete (DirEntriesCache<FILE_ID_BOTH_DIR_INFORMATION>*)cache;
    } else if (info.second == FileIdFullDirectoryInformation) {
        delete (DirEntriesCache<FILE_ID_FULL_DIR_INFORMATION>*)cache;
    } else {
        printf("Unknown cache type: %d\n", info.second);
    }
}

VFS::~VFS() {
    Uninit();
    LOG("dir_entries_cache size: %zu\n", dir_entries_cache.size());
    for (auto& [info, cache]: dir_entries_cache) {
        CleanupCache(info, cache);
    }
    LOG("handle_map size: %zu\n", handle_map.size());
    for (auto& [handle, _]: handle_map) {
        delete (Xp3File*)handle;
    }
    LOG("dir_handles size: %zu\n", dir_handles.size());
    for (auto& handle: dir_handles) {
        delete (DirEntry*)handle;
    }
    for (auto archive : archives) {
        delete archive;
    }
#if WINVFS_LOGGING
    if (logFile) {
        fclose(logFile);
    }
#endif
}

bool VFS::AddArchive(const char* path) {
    auto archive = new Xp3Archive(path);
    if (!archive->ReadIndex()) {
        delete archive;
        return false;
    }
    archives.push_front(archive);
    for (auto entry: archive->files) {
        auto name = str_util::str_replace(entry.filename, "/", "\\");
        if (files.find(name) == files.end()) {
            AddEntry(str_util::str_replace(entry.filename, "\\", "/"));
        }
        files[name] = std::pair(entry, archive);
    }
    return true;
}

bool VFS::Init() {
    auto hModule = GetModuleHandleW(L"ntdll.dll");
    if (!hModule) {
        hModule = LoadLibraryW(L"ntdll.dll");
        if (!hModule) {
            return false;
        }
    }
    Real_NtCreateFile = (decltype(Real_NtCreateFile))GetProcAddress(hModule, "NtCreateFile");
    if (!Real_NtCreateFile) {
        return false;
    }
    Real_NtClose = (decltype(Real_NtClose))GetProcAddress(hModule, "NtClose");
    if (!Real_NtClose) {
        return false;
    }
    Real_NtReadFile = (decltype(Real_NtReadFile))GetProcAddress(hModule, "NtReadFile");
    if (!Real_NtReadFile) {
        return false;
    }
    Real_NtQueryInformationFile = (decltype(Real_NtQueryInformationFile))GetProcAddress(hModule, "NtQueryInformationFile");
    if (!Real_NtQueryInformationFile) {
        return false;
    }
    Real_NtQueryVolumeInformationFile = (decltype(Real_NtQueryVolumeInformationFile))GetProcAddress(hModule, "NtQueryVolumeInformationFile");
    if (!Real_NtQueryVolumeInformationFile) {
        return false;
    }
    Real_NtQueryObject = (decltype(Real_NtQueryObject))GetProcAddress(hModule, "NtQueryObject");
    if (!Real_NtQueryObject) {
        return false;
    }
    Real_NtQueryAttributesFile = (decltype(Real_NtQueryAttributesFile))GetProcAddress(hModule, "NtQueryAttributesFile");
    if (!Real_NtQueryAttributesFile) {
        return false;
    }
    Real_NtSetInformationFile = (decltype(Real_NtSetInformationFile))GetProcAddress(hModule, "NtSetInformationFile");
    if (!Real_NtSetInformationFile) {
        return false;
    }
    Real_NtOpenFile = (decltype(Real_NtOpenFile))GetProcAddress(hModule, "NtOpenFile");
    if (!Real_NtOpenFile) {
        return false;
    }
    Real_NtCreateSection = (decltype(Real_NtCreateSection))GetProcAddress(hModule, "NtCreateSection");
    if (!Real_NtCreateSection) {
        return false;
    }
    Real_NtMapViewOfSection = (decltype(Real_NtMapViewOfSection))GetProcAddress(hModule, "NtMapViewOfSection");
    if (!Real_NtMapViewOfSection) {
        return false;
    }
    Real_NtUnmapViewOfSection = (decltype(Real_NtUnmapViewOfSection))GetProcAddress(hModule, "NtUnmapViewOfSection");
    if (!Real_NtUnmapViewOfSection) {
        return false;
    }
    Real_NtDuplicateObject = (decltype(Real_NtDuplicateObject))GetProcAddress(hModule, "NtDuplicateObject");
    if (!Real_NtDuplicateObject) {
        return false;
    }
    Real_NtQueryFullAttributesFile = (decltype(Real_NtQueryFullAttributesFile))GetProcAddress(hModule, "NtQueryFullAttributesFile");
    if (!Real_NtQueryFullAttributesFile) {
        return false;
    }
    Real_NtQueryDirectoryFile = (decltype(Real_NtQueryDirectoryFile))GetProcAddress(hModule, "NtQueryDirectoryFile");
    if (!Real_NtQueryDirectoryFile) {
        return false;
    }
    Real_NtQueryDirectoryFileEx = (decltype(Real_NtQueryDirectoryFileEx))GetProcAddress(hModule, "NtQueryDirectoryFileEx");
    Real_RtlIsNameInExpression = (decltype(Real_RtlIsNameInExpression))GetProcAddress(hModule, "RtlIsNameInExpression");
    if (!Real_RtlIsNameInExpression) {
        return false;
    }
    Real_NtWriteFile = (decltype(Real_NtWriteFile))GetProcAddress(hModule, "NtWriteFile");
    if (!Real_NtWriteFile) {
        return false;
    }
    Real_NtSetIoCompletion = (decltype(Real_NtSetIoCompletion))GetProcAddress(hModule, "NtSetIoCompletion");
    if (!Real_NtSetIoCompletion) {
        return false;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&Real_NtCreateFile, Hooked_NtCreateFile);
    DetourAttach(&Real_NtClose, Hooked_NtClose);
    DetourAttach(&Real_NtReadFile, Hooked_NtReadFile);
    DetourAttach(&Real_NtQueryInformationFile, Hooked_NtQueryInformationFile);
    DetourAttach(&Real_NtQueryVolumeInformationFile, Hooked_NtQueryVolumeInformationFile);
    DetourAttach(&Real_NtQueryObject, Hooked_NtQueryObject);
    DetourAttach(&Real_NtQueryAttributesFile, Hooked_NtQueryAttributesFile);
    DetourAttach(&Real_NtSetInformationFile, Hooked_NtSetInformationFile);
    DetourAttach(&Real_NtOpenFile, Hooked_NtOpenFile);
    DetourAttach(&Real_NtCreateSection, Hooked_NtCreateSection);
    DetourAttach(&Real_NtMapViewOfSection, Hooked_NtMapViewOfSection);
    DetourAttach(&Real_NtQueryFullAttributesFile, Hooked_NtQueryFullAttributesFile);
    DetourAttach(&Real_NtQueryDirectoryFile, Hooked_NtQueryDirectoryFile);
    if (Real_NtQueryDirectoryFileEx) {
        DetourAttach(&Real_NtQueryDirectoryFileEx, Hooked_NtQueryDirectoryFileEx);
    }
    DetourAttach(&Real_NtWriteFile, Hooked_NtWriteFile);
    DetourTransactionCommit();
    inited = true;
    return true;
}

bool VFS::GetEntry(std::string& path, FileEntry& entry, Xp3Archive*& archive) {
    std::string rPath;
    if (str_util::str_startswith(path, dos_path)) {
        rPath = path.substr(dos_path.length());
    } else if (str_util::str_startswith(path, dos_system_path)) {
        rPath = path.substr(dos_system_path.length());
    } else if (str_util::str_startswith(path, guid_path)) {
        rPath = path.substr(guid_path.length());
    } else if (str_util::str_startswith(path, nt_path)) {
        rPath = path.substr(nt_path.length());
    } else if (str_util::str_startswith(path, base_path)) {
        rPath = path.substr(base_path.length());
    } else {
        return false;
    }
    auto it = files.find(rPath);
    if (it == files.end()) {
        return false;
    }
    entry = it->second.first;
    archive = it->second.second;
    return true;
}

HANDLE VFS::OpenFile(FileEntry entry, Xp3Archive* archive) {
    auto file = archive->OpenFile(entry);
    if (!file) {
        return INVALID_HANDLE_VALUE;
    }
    auto hFile = (HANDLE)file;
    handle_map[hFile] = std::pair(entry, archive);
    return hFile;
}

bool VFS::ContainsFile(HANDLE file) {
    return handle_map.find(file) != handle_map.end();
}

void VFS::CloseFile(HANDLE file) {
    auto it = handle_map.find(file);
    if (it != handle_map.end()) {
        handle_map.erase(it);
    }
    auto it2 = complete_infos.find(file);
    if (it2 != complete_infos.end()) {
        complete_infos.erase(it2);
    }
    delete (Xp3File*)file;
}

#if WINVFS_LOGGING
void VFS::Log(const char* format, ...) {
    if (!logFile) return;
    va_list args;
    va_start(args, format);
    vfprintf(logFile, format, args);
    va_end(args);
    fflush(logFile);
}
#endif

Xp3File* VFS::GetFile(HANDLE file) {
    auto it = handle_map.find(file);
    if (it != handle_map.end()) {
        return (Xp3File*)file;
    }
    return nullptr;
}

bool VFS::GetFileInfo(HANDLE file, FileEntry& entry, Xp3Archive*& archive) {
    auto it = handle_map.find(file);
    if (it != handle_map.end()) {
        entry = it->second.first;
        archive = it->second.second;
        return true;
    }
    return false;
}

std::string VFS::GetRootPath(std::string& path) {
    auto p = fileop::join(base_path, str_util::str_replace(path, "/", "\\"));
    // Remove the leading part of the path to get a relative path
    return p.substr(2, p.length() - 2);
}

bool VFS::Uninit() {
    if (!inited) {
        return false;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&Real_NtCreateFile, Hooked_NtCreateFile);
    DetourDetach(&Real_NtClose, Hooked_NtClose);
    DetourDetach(&Real_NtReadFile, Hooked_NtReadFile);
    DetourDetach(&Real_NtQueryInformationFile, Hooked_NtQueryInformationFile);
    DetourDetach(&Real_NtQueryVolumeInformationFile, Hooked_NtQueryVolumeInformationFile);
    DetourDetach(&Real_NtQueryObject, Hooked_NtQueryObject);
    DetourDetach(&Real_NtQueryAttributesFile, Hooked_NtQueryAttributesFile);
    DetourDetach(&Real_NtSetInformationFile, Hooked_NtSetInformationFile);
    DetourDetach(&Real_NtOpenFile, Hooked_NtOpenFile);
    DetourDetach(&Real_NtCreateSection, Hooked_NtCreateSection);
    DetourDetach(&Real_NtMapViewOfSection, Hooked_NtMapViewOfSection);
    DetourDetach(&Real_NtQueryFullAttributesFile, Hooked_NtQueryFullAttributesFile);
    DetourDetach(&Real_NtQueryDirectoryFile, Hooked_NtQueryDirectoryFile);
    if (Real_NtQueryDirectoryFileEx) {
        DetourDetach(&Real_NtQueryDirectoryFileEx, Hooked_NtQueryDirectoryFileEx);
    }
    DetourDetach(&Real_NtWriteFile, Hooked_NtWriteFile);
    DetourTransactionCommit();
    inited = false;
    return true;
}

std::string VFS::GetNtPath(std::string& path) {
    return nt_path + str_util::str_replace(path, "/", "\\");
}

#if WINVFS_LOGGING
void VFS::AddTrace(HANDLE hFile) {
    trace_handles.insert(hFile);
}

bool VFS::InTrace(HANDLE hFile) {
    return trace_handles.find(hFile) != trace_handles.end();
}
#endif

void VFS::AddSectionHandle(HANDLE hSection, std::pair<FileEntry, Xp3Archive*> fileInfo) {
    section_handles[hSection] = fileInfo;
}

bool VFS::IsSectionHandle(HANDLE hSection) {
    return section_handles.find(hSection) != section_handles.end();
}

void VFS::RemoveSectionHandle(HANDLE hSection) {
    section_handles.erase(hSection);
}

bool VFS::GetSectionInfo(HANDLE hSection, FileEntry& entry, Xp3Archive*& archive) {
    auto it = section_handles.find(hSection);
    if (it != section_handles.end()) {
        entry = it->second.first;
        archive = it->second.second;
        return true;
    }
    return false;
}

bool VFS::IsRootDirectory(std::string& path) {
    if (path == dos_path || path == dos_system_path || path == guid_path || path == nt_path || path == base_path) {
        return true;
    }
    std::string bPath = path + "\\";
    return bPath == dos_path || bPath == dos_system_path || bPath == guid_path || bPath == nt_path || bPath == base_path;
}

void VFS::AddExistedDirHandle(HANDLE hDir, std::string path) {
    existed_dir_handles[hDir] = path;
}

std::string VFS::GetExistedDirHandlePath(HANDLE hDir) {
    auto it = existed_dir_handles.find(hDir);
    if (it != existed_dir_handles.end()) {
        return it->second;
    }
    return "";
}

bool VFS::IsExistedDirHandle(HANDLE hDir) {
    return existed_dir_handles.find(hDir) != existed_dir_handles.end();
}

void VFS::RemoveExistedDirHandle(HANDLE hDir) {
    existed_dir_handles.erase(hDir);
    auto it = dir_entries_cache.begin();
    while (it != dir_entries_cache.end()) {
        if (it->first.first == hDir) {
            LOG("Removing cache for handle: %p, type: %d\n", hDir, it->first.second);
            CleanupCache(it->first, it->second);
            it = dir_entries_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void VFS::AddEntry(std::string path) {
    size_t start = 0;
    auto ind = path.find_first_of('/');
    if (ind == std::string::npos) {
        if (directoryEntries.find("/") == directoryEntries.end()) {
            directoryEntries["/"] = std::vector<std::string>();
        }
        directoryEntries["/"].push_back(path);
        return;
    }
    std::string fDir = "/";
    while (ind != std::string::npos) {
        std::string dir = path.substr(start, ind - start + 1); // Include the '/' in the directory name
        start = ind + 1;
        std::string nDir = fDir + dir;
        if (directoryEntries.find(nDir) == directoryEntries.end()) {
            directoryEntries[nDir] = std::vector<std::string>();
            directoryEntries[fDir].push_back(dir);
        }
        fDir = nDir;
        ind = path.find_first_of('/', start);
    }
    std::string filename = path.substr(start);
    if (directoryEntries.find(fDir) == directoryEntries.end()) {
        directoryEntries[fDir] = std::vector<std::string>();
    }
    directoryEntries[fDir].push_back(filename);
}

static std::vector<std::string> emptyVector;

std::vector<std::string>& VFS::GetDirectoryEntries(std::string path) {
    auto it = directoryEntries.find(path);
    if (it != directoryEntries.end()) {
        return it->second;
    }
    return emptyVector;
}

bool VFS::GetFileEntry(std::string path, FileEntry& entry) {
    path = str_util::str_replace(path, "/", "\\");
    auto it = files.find(path);
    if (it != files.end()) {
        entry = it->second.first;
        return true;
    }
    return false;
}

bool VFS::HasDirectory(std::string path) {
    std::string rPath;
    if (str_util::str_startswith(path, dos_path)) {
        rPath = path.substr(dos_path.length());
    } else if (str_util::str_startswith(path, dos_system_path)) {
        rPath = path.substr(dos_system_path.length());
    } else if (str_util::str_startswith(path, guid_path)) {
        rPath = path.substr(guid_path.length());
    } else if (str_util::str_startswith(path, nt_path)) {
        rPath = path.substr(nt_path.length());
    } else if (str_util::str_startswith(path, base_path)) {
        rPath = path.substr(base_path.length());
    } else {
        return false;
    }
    rPath = str_util::str_replace(rPath, "\\", "/");
    rPath.insert(0, "/");
    if (rPath.back() != '/') {
        rPath.push_back('/');
    }
    return directoryEntries.find(rPath) != directoryEntries.end();
}

bool VFS::GetDirectoryName(std::string path, std::string& name) {
    std::string rPath;
    if (str_util::str_startswith(path, dos_path)) {
        rPath = path.substr(dos_path.length());
    } else if (str_util::str_startswith(path, dos_system_path)) {
        rPath = path.substr(dos_system_path.length());
    } else if (str_util::str_startswith(path, guid_path)) {
        rPath = path.substr(guid_path.length());
    } else if (str_util::str_startswith(path, nt_path)) {
        rPath = path.substr(nt_path.length());
    } else if (str_util::str_startswith(path, base_path)) {
        rPath = path.substr(base_path.length());
    } else {
        return false;
    }
    rPath = str_util::str_replace(rPath, "\\", "/");
    rPath.insert(0, "/");
    if (rPath.back() != '/') {
        rPath.push_back('/');
    }
    bool ok = directoryEntries.find(rPath) != directoryEntries.end();
    if (ok) {
        name = rPath;
    }
    return ok;
}

HANDLE VFS::OpenDirectory(std::string path) {
    auto hDir = new DirEntry(path);
    if (!hDir) {
        return INVALID_HANDLE_VALUE;
    }
    auto h = (HANDLE)hDir;
    AddDirectoryHandle(h);
    return h;
}
 
void VFS::AddDirectoryHandle(HANDLE hDir) {
    dir_handles.insert(hDir);
}

bool VFS::IsDirectoryHandle(HANDLE hDir) {
    return dir_handles.find(hDir) != dir_handles.end();
}

void VFS::RemoveDirectoryHandle(HANDLE hDir) {
    dir_handles.erase(hDir);
    auto it = dir_entries_cache.begin();
    while (it != dir_entries_cache.end()) {
        if (it->first.first == hDir) {
            LOG("Removing cache for handle: %p, type: %d\n", hDir, it->first.second);
            CleanupCache(it->first, it->second);
            it = dir_entries_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void VFS::CloseDirectory(HANDLE hDir) {
    RemoveDirectoryHandle(hDir);
    delete (DirEntry*)hDir;
}

bool VFS::AddArchive(std::string path) {
    return AddArchive(path.c_str());
}

bool VFS::AddArchive(std::wstring path) {
    std::string p;
    if (!wchar_util::wstr_to_str(p, path, CP_UTF8)) {
        return false;
    }
    return AddArchive(p.c_str());
}

void VFS::AddArchiveWithErrorMsg(const char* path) {
    if (!AddArchive(path)) {
        std::wstring wpath;
        if (!wchar_util::str_to_wstr(wpath, path, CP_UTF8)) {
            MessageBoxW(NULL, L"无法打开资源文件。请检查资源文件是否完整", L"错误", MB_ICONERROR);
            ExitProcess(1);
            return;
        }
        std::wstring wmsg = L"无法打开 " + wpath + L"。请检查文件是否存在";
        MessageBoxW(NULL, wmsg.c_str(), L"错误", MB_ICONERROR);
        ExitProcess(1);
        return;
    }
}

void VFS::AddArchiveWithErrorMsg(std::string path) {
    AddArchiveWithErrorMsg(path.c_str());
}

void VFS::AddArchiveWithErrorMsg(std::wstring path) {
    std::string p;
    if (wchar_util::wstr_to_str(p, path, CP_UTF8)) {
        AddArchiveWithErrorMsg(p.c_str());
    } else {
        std::wstring wmsg = L"无法转换编码，文件" + path + L"无法打开";
        MessageBoxW(NULL, wmsg.c_str(), L"错误", MB_ICONERROR);
        ExitProcess(1);
    }
}

PCompleteInfo VFS::GetCompletionInfo(HANDLE hFile) {
    auto it = complete_infos.find(hFile);
    if (it != complete_infos.end()) {
        return &it->second;
    }
    return nullptr;
}

void VFS::SetCompletionInfo(HANDLE hFile, CompleteInfo info) {
    complete_infos[hFile] = info;
}
