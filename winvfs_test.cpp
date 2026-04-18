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
    auto cls = CloseHandle(file);
    if (!cls) {
        printf("Failed to close file handle\n");
    }
    return 0;
}
