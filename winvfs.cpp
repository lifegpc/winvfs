#include "winvfs.hpp"
#include <Windows.h>
#include "wchar_util.h"
#include "fileop.h"
#include "detours.h"
#include <winternl.h>
// #include <ntstatus.h>

static VFS g_vfs;

VFS& GetGlobalVFS() {
    return g_vfs;
}

#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)

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
            g_vfs.Log("NtCreateFile: %s\n", filepath.c_str());
            FileEntry entry;
            Xp3Archive* archive;
            if (g_vfs.GetEntry(filepath, entry, archive)) {
                g_vfs.Log("File found in VFS: %s\n", filepath.c_str());
                auto hFile = g_vfs.OpenFile(entry, archive);
                if (hFile != INVALID_HANDLE_VALUE) {
                    *FileHandle = hFile;
                    IoStatusBlock->Status = FILE_OPENED;
                    IoStatusBlock->Information = 0;
                    g_vfs.Log("File opened successfully: %s. %p\n", filepath.c_str(), hFile);
                    return STATUS_SUCCESS;
                }
            }
        }
    }
    return Real_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtClose(
    IN HANDLE Handle
) {
    if (g_vfs.ContainsFile(Handle)) {
        g_vfs.Log("NtClose: %p\n", Handle);
        g_vfs.CloseFile(Handle);
        return STATUS_SUCCESS;
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
        g_vfs.Log("NtReadFile: %p, Length: %lu, ByteOffset: %lld\n", FileHandle, Length, ByteOffset ? ByteOffset->QuadPart : -1);
        size_t bytesRead;
        if (ByteOffset) {
            bytesRead = file->read_at((uint8_t*)Buffer, Length, ByteOffset->QuadPart);
        } else {
            bytesRead = file->read((uint8_t*)Buffer, Length);
        }
        g_vfs.Log("Bytes read: %llu\n", bytesRead);
        if (bytesRead == 0) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        } else {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = bytesRead;
        }
        return STATUS_SUCCESS;
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
        g_vfs.Log("NtQueryInformationFile: %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
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
    }
    if (g_vfs.InTrace(FileHandle)) {
        g_vfs.Log("NtQueryInformationFile (trace): %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
        auto result = Real_NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
        if (result == STATUS_SUCCESS && IoStatusBlock->Status == STATUS_SUCCESS) {
            if (FileInformationClass == FileNameInformation) {
                FILE_NAME_INFORMATION* info = (FILE_NAME_INFORMATION*)FileInformation;
                std::wstring wFilename(info->FileName, info->FileNameLength / sizeof(WCHAR));
                std::string filename;
                wchar_util::wstr_to_str(filename, wFilename, CP_UTF8);
                g_vfs.Log("Returned file name: %s\n", filename.c_str());
            }
        }
        return result;
    }
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
    if (g_vfs.GetFileInfo(FileHandle, entry, archive)) {
        g_vfs.Log("NtQueryVolumeInformationFile: %p, FsInformationClass: %d\n", FileHandle, FsInformationClass);
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
    if (g_vfs.InTrace(FileHandle)) {
        g_vfs.Log("NtQueryVolumeInformationFile (trace): %p, FsInformationClass: %d\n", FileHandle, FsInformationClass);
        auto result = Real_NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass);
        return result;
    }
    return Real_NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass);
}

__kernel_entry NTSTATUS NTAPI Hooked_NtQueryObject(
    IN HANDLE Handle OPTIONAL,
    IN OBJECT_INFORMATION_CLASS ObjectInformationClass,
    OUT PVOID ObjectInformation OPTIONAL,
    IN ULONG ObjectInformationLength,
    OUT PULONG ReturnLength OPTIONAL
) {
    FileEntry entry;
    Xp3Archive* archive;
    if (Handle != INVALID_HANDLE_VALUE && ObjectInformation && g_vfs.GetFileInfo(Handle, entry, archive)) {
        g_vfs.Log("NtQueryObject: %p, ObjectInformationClass: %d\n", Handle, ObjectInformationClass);
        if (ObjectInformationClass == ObjectNameInformation) {
            std::string ntpath = g_vfs.GetNtPath(entry.filename);
            g_vfs.Log("Object name for handle %p: %s\n", Handle, ntpath.c_str());
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
    }
    if (g_vfs.InTrace(Handle)) {
        g_vfs.Log("NtQueryObject (trace): %p, ObjectInformationClass: %d\n", Handle, ObjectInformationClass);
    }
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
            g_vfs.Log("NtQueryAttributesFile: %s\n", filepath.c_str());
            FileEntry entry;
            Xp3Archive* archive;
            if (g_vfs.GetEntry(filepath, entry, archive)) {
                g_vfs.Log("File found in VFS: %s\n", filepath.c_str());
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
        g_vfs.Log("NtSetInformationFile: %p, FileInformationClass: %d\n", FileHandle, FileInformationClass);
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
        }
    }
    return Real_NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
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

VFS::~VFS() {
    Uninit();
    for (auto archive : archives) {
        delete archive;
    }
    if (logFile) {
        fclose(logFile);
    }
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
    delete (Xp3File*)file;
}

void VFS::Log(const char* format, ...) {
    if (!logFile) return;
    va_list args;
    va_start(args, format);
    vfprintf(logFile, format, args);
    va_end(args);
    fflush(logFile);
}

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
    DetourTransactionCommit();
    inited = false;
    return true;
}

std::string VFS::GetNtPath(std::string& path) {
    return nt_path + str_util::str_replace(path, "/", "\\");
}

void VFS::AddTrace(HANDLE hFile) {
    trace_handles.insert(hFile);
}

bool VFS::InTrace(HANDLE hFile) {
    return trace_handles.find(hFile) != trace_handles.end();
}
