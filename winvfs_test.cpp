#include "winvfs.hpp"
#include "fileop.h"
#include "err.h"
#include "wchar_util.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);
    {
        auto& g_vfs = GetGlobalVFS();
        g_vfs.logFile = fopen("winvfs.log", "w");
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
    return 0;
}
