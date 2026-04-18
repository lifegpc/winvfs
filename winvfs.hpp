#pragma once
#include <string>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include "xp3.h"
#include "str_util.h"
#include <Windows.h>
#include <stdio.h>

struct CaseInsensitiveHash {
    size_t operator()(const std::string& str) const {
        // 创建字符串的小写副本
        std::string lowercaseStr = str_util::tolower(str);
        
        // 对小写字符串使用标准哈希函数
        return std::hash<std::string>{}(lowercaseStr);
    }
};

// 比较函数，忽略大小写
struct CaseInsensitiveEqual {
    bool operator()(const std::string& left, const std::string& right) const {
        return left.size() == right.size() &&
               std::equal(left.begin(), left.end(), right.begin(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    }
};

class VFS {
public:
    VFS();
    ~VFS();
    bool AddArchive(const char* path);
    bool Init();
    bool Uninit();
    // Follow function used in Hooked code.
    bool GetEntry(std::string& path, FileEntry& entry, Xp3Archive*& archive);
    HANDLE OpenFile(FileEntry entry, Xp3Archive* archive);
    bool ContainsFile(HANDLE file);
    void CloseFile(HANDLE file);
    void Log(const char* format, ...);
    Xp3File* GetFile(HANDLE file);
    bool GetFileInfo(HANDLE file, FileEntry& entry, Xp3Archive*& archive);
    FILE* logFile = nullptr;
    std::string GetRootPath(std::string& path);
    std::string GetNtPath(std::string& path);
    void AddTrace(HANDLE hFile);
    bool InTrace(HANDLE hFile);
    void AddSectionHandle(HANDLE hSection, std::pair<FileEntry, Xp3Archive*> fileInfo);
    bool GetSectionInfo(HANDLE hSection, FileEntry& entry, Xp3Archive*& archive);
    bool IsSectionHandle(HANDLE hSection);
    void RemoveSectionHandle(HANDLE hSection);
private:
    bool inited = false;
    std::string base_path;
    std::string nt_path;
    std::string dos_path;
    std::string dos_system_path;
    std::string guid_path;
    std::list<Xp3Archive*> archives;
    std::unordered_map<std::string, std::pair<FileEntry, Xp3Archive*>, CaseInsensitiveHash, CaseInsensitiveEqual> files;
    std::unordered_map<HANDLE, std::pair<FileEntry, Xp3Archive*>> handle_map;
    std::unordered_set<HANDLE> trace_handles;
    std::unordered_map<HANDLE, std::pair<FileEntry, Xp3Archive*>> section_handles;
};

VFS& GetGlobalVFS();
