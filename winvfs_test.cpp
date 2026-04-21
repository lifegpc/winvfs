#include "winvfs.hpp"
#include "fileop.h"
#include "err.h"
#include "wchar_util.h"

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

void ListEntry(std::wstring name, std::wstring filename) {
    auto QueryDirectoryFile = (NtQueryDirectoryFile_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile");
    HANDLE hDir = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hDir == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to open directory: %s\n", errMsg.c_str());
        return;
    }
    BYTE buffer[4096];
    IO_STATUS_BLOCK ioStatusBlock;
    UNICODE_STRING unicodeFilename;
    unicodeFilename.Length = (USHORT)(filename.size() * sizeof(WCHAR));
    unicodeFilename.MaximumLength = (USHORT)((filename.size() + 1) * sizeof(WCHAR));
    unicodeFilename.Buffer = (PWSTR)filename.c_str();
    NTSTATUS status;
    do {
        status = QueryDirectoryFile(hDir, NULL, NULL, NULL, &ioStatusBlock, buffer, sizeof(buffer), (FILE_INFORMATION_CLASS)FileBothDirectoryInformation, FALSE, &unicodeFilename, FALSE);
        if (status == 0) {
            PFILE_BOTH_DIR_INFORMATION dirInfo = (PFILE_BOTH_DIR_INFORMATION)buffer;
            while (true) {
                std::wstring fileName(dirInfo->FileName, dirInfo->FileNameLength / sizeof(WCHAR));
                printf("Found file: %ws\n", fileName.c_str());
                printf("NextEntryOffset: %lu\n", dirInfo->NextEntryOffset);
                if (dirInfo->NextEntryOffset == 0) {
                    break;
                }
                dirInfo = (PFILE_BOTH_DIR_INFORMATION)((BYTE*)dirInfo + dirInfo->NextEntryOffset);
            }
        } else if (status != 0x80000006L) { // STATUS_NO_MORE_FILES
            std::string errMsg;
            err::get_winerror(errMsg, status);
            printf("Failed to query directory: %s\n", errMsg.c_str());
            CloseHandle(hDir);
            break;
        }
    } while (status == 0);
    CloseHandle(hDir);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    {
        auto& g_vfs = GetGlobalVFS();
#if WINVFS_LOGGING
        g_vfs.logFile = fopen("winvfs.log", "w");
#endif
        printf("Initializing VFS...\n");
        if (!g_vfs.AddArchive("winvfs.xp3")) {
            printf("Failed to add archive: winvfs.xp3\n");
            return 1;
        }
        if (!g_vfs.Init()) {
            printf("Failed to initialize VFS\n");
            return 1;
        }
    }
    printf("VFS initialized successfully\n");
    auto file = CreateFileW(L"hello.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_READ_ATTRIBUTES, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        printf("File handle: %p\n", file);
        char buffer[128];
        DWORD bytesRead;
        auto ok = ReadFile(file, buffer, 128, &bytesRead, NULL);
        if (ok) {
            printf("Read %lu bytes:\n", bytesRead);
            printf("%.*s\n", bytesRead, buffer);
            printf("\n");
        } else {
            printf("Failed to read file\n");
        }
        BY_HANDLE_FILE_INFORMATION fileInfo;
        if (GetFileInformationByHandle(file, &fileInfo)) {
            printf("File size: %lld bytes\n", ((LONGLONG)fileInfo.nFileSizeHigh << 32) | fileInfo.nFileSizeLow);
        } else {
            std::string errMsg;
            err::get_winerror(errMsg, GetLastError());
            printf("Failed to get file information: %s\n", errMsg.c_str());
        }
        wchar_t fullPath[MAX_PATH];
        DWORD ret = GetFinalPathNameByHandleW(file, fullPath, MAX_PATH, FILE_NAME_NORMALIZED);
        if (ret == 0) {
            std::string errMsg;
            err::get_winerror(errMsg, GetLastError());
            printf("Failed to get final path: %s\n", errMsg.c_str());
        } else {
            std::wstring wFullPath(fullPath, ret);
            std::string strFullPath;
            wchar_util::wstr_to_str(strFullPath, wFullPath, CP_UTF8);
            printf("Final path: %s\n", strFullPath.c_str());
        }
        ret = GetFinalPathNameByHandleW(file, fullPath, MAX_PATH, VOLUME_NAME_GUID);
        if (ret == 0) {
            std::string errMsg;
            err::get_winerror(errMsg, GetLastError());
            printf("Failed to get final path (GUID): %s\n", errMsg.c_str());
        } else {
            std::wstring wFullPath(fullPath, ret);
            std::string strFullPath;
            wchar_util::wstr_to_str(strFullPath, wFullPath, CP_UTF8);
            printf("Final path (GUID): %s\n", strFullPath.c_str());
        }
        auto fileType = GetFileType(file);
        printf("File type: %d\n", fileType);
        LARGE_INTEGER fileSize;
        if (GetFileSizeEx(file, &fileSize)) {
            printf("File size: %lld bytes\n", fileSize.QuadPart);
        } else {
            std::string errMsg;
            err::get_winerror(errMsg, GetLastError());
            printf("Failed to get file size: %s\n", errMsg.c_str());
        }
        auto mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (mapping == NULL) {
            std::string errMsg;
            err::get_winerror(errMsg, GetLastError());
            printf("Failed to create file mapping: %s\n", errMsg.c_str());
        } else {
            printf("File mapping created successfully: %p\n", mapping);
            auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (view == NULL) {
                std::string errMsg;
                err::get_winerror(errMsg, GetLastError());
                printf("Failed to map view of file: %s\n", errMsg.c_str());
            } else {
                printf("File mapped successfully: %p\n", view);
                printf("Mapped content:\n");
                printf("%.*s\n", (int)fileSize.QuadPart, (char*)view);
                UnmapViewOfFile(view);
            }
            CloseHandle(mapping);
        }
        auto cls = CloseHandle(file);
        if (!cls) {
            printf("Failed to close file handle\n");
        }
    }
    auto f = _wfopen(L"hello.txt", L"r");
    if (!f) {
        std::string errMsg;
        err::get_errno_message(errMsg, errno);
        printf("Failed to open file with fopen: %s\n", errMsg.c_str());
    } else {
        char buffer[128];
        size_t bytesRead = fread(buffer, 1, 128, f);
        if (bytesRead > 0) {
            printf("Read %zu bytes with fread:\n", bytesRead);
            printf("%.*s\n", (int)bytesRead, buffer);
            printf("\n");
        } else {
            printf("Failed to read file with fopen\n");
        }
        int64_t pos = _ftelli64(f);
        if (pos != -1) {
            printf("Current file position: %lld\n", pos);
        } else {
            std::string errMsg;
            err::get_errno_message(errMsg, errno);
            printf("Failed to get file position: %s\n", errMsg.c_str());
        }
        int result = _fseeki64(f, 0, SEEK_SET);
        if (result != 0) {
            std::string errMsg;
            err::get_errno_message(errMsg, errno);
            printf("Failed to seek file: %s\n", errMsg.c_str());
        } else {
            printf("File seeked to beginning successfully\n");
        }
        bytesRead = fread(buffer, 1, 128, f);
        if (bytesRead > 0) {
            printf("Read %zu bytes with fread after seeking:\n", bytesRead);
            printf("%.*s\n", (int)bytesRead, buffer);
            printf("\n");
        } else {
            printf("Failed to read file with fopen after seeking\n");
        }
        fclose(f);
    }
    auto attributes = GetFileAttributesW(L"hello.txt");
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to get file attributes: %s\n", errMsg.c_str());
    } else {
        printf("File attributes: 0x%08X\n", attributes);
    }
    if (fileop::exists("hello.txt")) {
        printf("fileop::exists: hello.txt File exists\n");
    }
    struct __stat64 fileStat;
    if (_wstat64(L"hello.txt", &fileStat) == 0) {
        printf("File size from _wstat64: %lld bytes\n", fileStat.st_size);
    } else {
        std::string errMsg;
        err::get_errno_message(errMsg, errno);
        printf("Failed to get file status with _wstat64: %s\n", errMsg.c_str());
    }
    auto hCurrentDir = CreateFileW(L"./", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hCurrentDir == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to open current directory: %s\n", errMsg.c_str());
    } else {
        wchar_t fullPath[MAX_PATH];
        DWORD ret = GetFinalPathNameByHandleW(hCurrentDir, fullPath, MAX_PATH, FILE_NAME_NORMALIZED);
        if (ret == 0) {
            std::string errMsg;
            err::get_winerror(errMsg, GetLastError());
            printf("Failed to get final path of current directory: %s\n", errMsg.c_str());
        } else {
            std::wstring wFullPath(fullPath, ret);
            std::string strFullPath;
            wchar_util::wstr_to_str(strFullPath, wFullPath, CP_UTF8);
            printf("Final path of current directory: %s\n", strFullPath.c_str());
        }
        CloseHandle(hCurrentDir);
    }
    WIN32_FIND_DATAW findData;
    auto hFind = FindFirstFileW(L"*", &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to find first file: %s\n", errMsg.c_str());
    } else {
        do {
            std::wstring wFilename(findData.cFileName);
            std::string filename;
            wchar_util::wstr_to_str(filename, wFilename, CP_UTF8);
            printf("Found file: %s, File Size: %llu, is_dir: %s\n", filename.c_str(), ((LONGLONG)findData.nFileSizeHigh << 32) | findData.nFileSizeLow, (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false");
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
    hFind = FindFirstFileW(L"*.txt", &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to find first file with pattern: %s\n", errMsg.c_str());
    } else {
        do {
            std::wstring wFilename(findData.cFileName);
            std::string filename;
            wchar_util::wstr_to_str(filename, wFilename, CP_UTF8);
            printf("Found file with pattern: %s, File Size: %llu, is_dir: %s\n", filename.c_str(), ((LONGLONG)findData.nFileSizeHigh << 32) | findData.nFileSizeLow, (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false");
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
    auto dirAttrs = GetFileAttributesW(L"data");
    if (dirAttrs == INVALID_FILE_ATTRIBUTES) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to get directory attributes: %s\n", errMsg.c_str());
    } else {
        printf("Directory attributes: 0x%08X\n", dirAttrs);
    }
    hFind = FindFirstFileW(L"meson-private\\*.txt", &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to find first file with pattern: %s\n", errMsg.c_str());
    } else {
        do {
            std::wstring wFilename(findData.cFileName);
            std::string filename;
            wchar_util::wstr_to_str(filename, wFilename, CP_UTF8);
            printf("Found file with pattern: %s, File Size: %llu, is_dir: %s\n", filename.c_str(), ((LONGLONG)findData.nFileSizeHigh << 32) | findData.nFileSizeLow, (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false");
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
    hFind = FindFirstFileW(L"meson-private\\*", &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to find first file with pattern: %s\n", errMsg.c_str());
    } else {
        FindClose(hFind);
    }
    if (fileop::exists("data")) {
        printf("fileop::exists: data Directory exists\n");
    }
    auto hDataDir = CreateFileW(L"data", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hDataDir == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to open data directory: %s\n", errMsg.c_str());
    } else {
        wchar_t fullPath[MAX_PATH];
        DWORD ret = GetFinalPathNameByHandleW(hDataDir, fullPath, MAX_PATH, VOLUME_NAME_NT);
        if (ret == 0) {
            std::string errMsg;
            err::get_winerror(errMsg, GetLastError());
            printf("Failed to get final path of data directory: %s\n", errMsg.c_str());
        } else {
            std::wstring wFullPath(fullPath, ret);
            std::string strFullPath;
            wchar_util::wstr_to_str(strFullPath, wFullPath, CP_UTF8);
            printf("Final path of data directory: %s\n", strFullPath.c_str());
        }
        CloseHandle(hDataDir);
    }
    bool existed;
    if (fileop::isdir("data", existed) && existed) {
        printf("fileop::isdir: data is a directory\n");
    }
    hFind = FindFirstFileW(L"data\\*", &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to find first file in data directory: %s\n", errMsg.c_str());
    } else {
        do {
            std::wstring wFilename(findData.cFileName);
            std::string filename;
            wchar_util::wstr_to_str(filename, wFilename, CP_UTF8);
            printf("Found file in data directory: %s, File Size: %llu, is_dir: %s\n", filename.c_str(), ((LONGLONG)findData.nFileSizeHigh << 32) | findData.nFileSizeLow, (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false");
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
    hFind = FindFirstFileW(L"data\\*", &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::string errMsg;
        err::get_winerror(errMsg, GetLastError());
        printf("Failed to find first file in data directory: %s\n", errMsg.c_str());
    } else {
        FindClose(hFind);
    }
    std::list<std::string> fileList;
    if (fileop::listdir("data/fonts", fileList)) {
        printf("Files in data/fonts directory:\n");
        for (const auto& file : fileList) {
            printf("%s\n", file.c_str());
        }
    }
    ListEntry(L"\\\\?\\D:\\git\\winvfs\\buildrel", L"*");
    ListEntry(L"meson-private", L"<.txt");
    return 0;
}
