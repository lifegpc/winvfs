#pragma once
#include <string>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "xp3.h"
#if WINVFS_ASAR
#include "asar.h"
#endif
#include "str_util.h"
#include <Windows.h>
#include <stdio.h>
#include <winternl.h>

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

struct PairHasher {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

template<typename T>
class DirEntriesCache {
public:
    ~DirEntriesCache() {
        for (auto entry : entries) {
            if (entry) delete entry;
        }
    }
    void push_back(T* entry, bool replace = false) {
        if (replace) {
            std::wstring filename(entry->FileName, entry->FileNameLength / sizeof(WCHAR));
            for (size_t i = pos; i < entries.size(); i++) {
                if (!entries[i]) continue;
                std::wstring existingFilename(entries[i]->FileName, entries[i]->FileNameLength / sizeof(WCHAR));
                if (existingFilename == filename) {
                    delete entries[i];
                    entries[i] = entry;
                    return;
                }
            }
        }
        entries.push_back(entry);
    }
    T* peek_one() {
        if (pos < entries.size()) {
            return entries[pos];
        }
        return nullptr;
    }
    void inc_one() {
        if (pos < entries.size()) {
            delete entries[pos];
            entries[pos] = nullptr;
            pos++;
        }
    }
    using size_type = typename std::vector<T*>::size_type;
    inline size_type size() const {
        return entries.size();
    }
    using iterator = typename std::vector<T*>::iterator;
    inline iterator begin() { return entries.begin(); }
    inline iterator end() { return entries.end(); }
    using const_iterator = typename std::vector<T*>::const_iterator;
    inline const_iterator begin() const { return entries.begin(); }
    inline const_iterator end() const { return entries.end(); }
    inline bool isEnd() const {
        return pos >= entries.size();
    }
    inline bool empty() const {
        return entries.empty();
    }
private:
    std::vector<T*> entries;
    size_t pos = 0;
};

class DirEntry {
public:
    DirEntry(std::string name) : name(name) {}
    std::string name;
};

typedef struct _CompleteInfo {
    HANDLE Port;
    PVOID  Key;
    ULONG  Flags;
    BOOLEAN Seted;
} CompleteInfo, *PCompleteInfo;

class VFS {
public:
    VFS();
    ~VFS();
    /**
     * Add a xp3 archive to vfs
     * @param path Path to a xp3 archive. UTF-8 or ANSI encoding are supported.
     */
    bool AddArchive(const char* path);
    /**
     * Add a xp3 archive to vfs
     * @param path Path to a xp3 archive. UTF-8 or ANSI encoding are supported.
     */
    bool AddArchive(std::string path);
    /**
     * Add a xp3 archive to vfs
     * @param path Path to a xp3 archive.
     */
    bool AddArchive(std::wstring path);
    /**
     * Add a xp3 archive to vfs. If failed, open a dialog for user and then exit program.
     * @param path Path to a xp3 archive. UTF-8 or ANSI encoding are supported.
     */
    void AddArchiveWithErrorMsg(const char* path);
    /**
     * Add a xp3 archive to vfs. If failed, open a dialog for user and then exit program.
     * @param path Path to a xp3 archive. UTF-8 or ANSI encoding are supported.
     */
    void AddArchiveWithErrorMsg(std::string path);
    /**
     * Add a xp3 archive to vfs. If failed, open a dialog for user and then exit program.
     * @param path Path to a xp3 archive.
     */
    void AddArchiveWithErrorMsg(std::wstring path);
#if WINVFS_MEMFILE
    /**
     * Add a file in memory to vfs.
     * @param name Virtual path of the file. only UTF-8 encoding is supported. It should not start with '/' or '\\'.
     * @param content Content of the file in bytes.
     */
    void AddMemFile(std::string name, std::vector<uint8_t> content);
    /**
     * Add a file in memory to vfs.
     * @param name Virtual path of the file. only UTF-8 encoding is supported. It should not start with '/' or '\\'.
     * @param content Content of the file in string.
     */
    void AddMemFile(std::string name, std::string content);
#endif
#if WINVFS_ASAR
    /**
     * Add a asar archive to vfs
     * @param path Path to a asar archive. UTF-8 or ANSI encoding are supported.
     */
    bool AddAsarArchive(const char* path);
    /**
     * Add a asar archive to vfs
     * @param path Path to a asar archive. UTF-8 or ANSI encoding are supported.
     */
    bool AddAsarArchive(std::string path);
    /**
     * Add a asar archive to vfs
     * @param path Path to a asar archive.
     */
    bool AddAsarArchive(std::wstring path);
    /**
     * Add a asar archive to vfs. If failed, open a dialog for user and then exit program.
     * @param path Path to a asar archive. UTF-8 or ANSI encoding are supported.
     */
    void AddAsarArchiveWithErrorMsg(const char* path);
    /**
     * Add a asar archive to vfs. If failed, open a dialog for user and then exit program.
     * @param path Path to a asar archive. UTF-8 or ANSI encoding are supported.
     */
    void AddAsarArchiveWithErrorMsg(std::string path);
    /**
     * Add a asar archive to vfs. If failed, open a dialog for user and then exit program.
     * @param path Path to a asar archive.
     */
    void AddAsarArchiveWithErrorMsg(std::wstring path);
#endif
    bool Init();
    bool Uninit();
    // Follow function used in Hooked code.
    bool GetEntry(std::string& path, FileEntry& entry, Xp3Archive*& archive);
    HANDLE OpenFile(FileEntry entry, Xp3Archive* archive);
    bool ContainsFile(HANDLE file);
    void CloseFile(HANDLE file);
#if WINVFS_LOGGING
    void Log(const char* format, ...);
#endif
    Xp3File* GetFile(HANDLE file);
    bool GetFileInfo(HANDLE file, FileEntry& entry, Xp3Archive*& archive);
#if WINVFS_LOGGING
    FILE* logFile = nullptr;
#endif
    std::string GetRootPath(std::string& path);
    std::string GetNtPath(std::string& path);
#if WINVFS_LOGGING
    void AddTrace(HANDLE hFile);
    bool InTrace(HANDLE hFile);
#endif
    void AddSectionHandle(HANDLE hSection, std::pair<FileEntry, Xp3Archive*> fileInfo);
    bool GetSectionInfo(HANDLE hSection, FileEntry& entry, Xp3Archive*& archive);
    bool IsSectionHandle(HANDLE hSection);
    void RemoveSectionHandle(HANDLE hSection);
    bool IsRootDirectory(std::string& path);
    void AddExistedDirHandle(HANDLE hDir, std::string path);
    std::string GetExistedDirHandlePath(HANDLE hDir);
    bool IsExistedDirHandle(HANDLE hDir);
    void RemoveExistedDirHandle(HANDLE hDir);
    template <typename T>
    void AddDirEntriesCache(HANDLE hDir, FILE_INFORMATION_CLASS infoClass, DirEntriesCache<T>* cache) {
        dir_entries_cache[std::make_pair(hDir, infoClass)] = cache;
    }
    template <typename T>
    DirEntriesCache<T>* GetDirEntriesCache(HANDLE hDir, FILE_INFORMATION_CLASS infoClass) {
        auto it = dir_entries_cache.find(std::make_pair(hDir, infoClass));
        if (it != dir_entries_cache.end()) {
            return (DirEntriesCache<T>*)it->second;
        }
        return nullptr;
    }
    template <typename T>
    void RemoveDirEntriesCache(HANDLE hDir, FILE_INFORMATION_CLASS infoClass) {
        auto it = dir_entries_cache.find(std::make_pair(hDir, infoClass));
        if (it != dir_entries_cache.end()) {
            delete (DirEntriesCache<T>*)it->second;
            dir_entries_cache.erase(it);
        }
    }
    std::vector<std::string>& GetDirectoryEntries(std::string path);
    bool GetFileEntry(std::string path, FileEntry& entry);
    bool HasDirectory(std::string path);
    bool GetDirectoryName(std::string path, std::string& name);
    HANDLE OpenDirectory(std::string path);
    void CloseDirectory(HANDLE hDir);
    void AddDirectoryHandle(HANDLE hDir);
    bool IsDirectoryHandle(HANDLE hDir);
    void RemoveDirectoryHandle(HANDLE hDir);
    PCompleteInfo GetCompletionInfo(HANDLE hFile);
    void SetCompletionInfo(HANDLE hFile, CompleteInfo info);
#if WINVFS_MEMFILE
    bool GetMemEntry(std::string& path, size_t& size);
    bool GetMemFileEntry(std::string& path, size_t& size);
    bool GetMemFileInfo(HANDLE hFile, size_t& size, std::string& name);
    bool IsMemFile(std::string& path);
    bool IsMemFileHandle(HANDLE hFile);
    HANDLE OpenMemFile(std::string path);
    void CloseMemFile(HANDLE hFile);
    ReadStream* GetMemFile(HANDLE hFile);
#endif
#if WINVFS_ASAR
    bool GetAsarEntry(std::string& path, asar::FileEntry& entry, asar::Archive*& archive);
    bool GetAsarFileEntry(std::string& path, asar::FileEntry& entry);
    bool GetAsarFileInfo(HANDLE hFile, asar::FileEntry& entry);
    bool IsAsarFile(std::string& path);
    bool IsAsarFileHandle(HANDLE hFile);
    HANDLE OpenAsarFile(std::string path);
    void CloseAsarFile(HANDLE hFile);
    asar::File* GetAsarFile(HANDLE hFile);
#endif
private:
    void AddEntry(std::string path);
    bool inited = false;
    std::string base_path;
    std::string nt_path;
    std::string dos_path;
    std::string dos_system_path;
    std::string guid_path;
    std::list<Xp3Archive*> archives;
    std::unordered_map<std::string, std::pair<FileEntry, Xp3Archive*>, CaseInsensitiveHash, CaseInsensitiveEqual> files;
    std::unordered_map<HANDLE, std::pair<FileEntry, Xp3Archive*>> handle_map;
#if WINVFS_LOGGING
    std::unordered_set<HANDLE> trace_handles;
#endif
    std::unordered_map<HANDLE, std::pair<FileEntry, Xp3Archive*>> section_handles;
    std::unordered_map<HANDLE, std::string> existed_dir_handles;
    // second is DirEntriesCache<T>* , T is based on FILE_INFORMATION_CLASS
    std::unordered_map<std::pair<HANDLE, FILE_INFORMATION_CLASS>, void*, PairHasher> dir_entries_cache;
    std::unordered_map<std::string, std::vector<std::string>, CaseInsensitiveHash, CaseInsensitiveEqual> directoryEntries;
    std::unordered_set<std::string, CaseInsensitiveHash, CaseInsensitiveEqual> addedEntries;
    std::unordered_set<HANDLE> dir_handles;
    std::unordered_map<HANDLE, CompleteInfo> complete_infos;
#if WINVFS_MEMFILE
    std::unordered_map<std::string, std::vector<uint8_t>, CaseInsensitiveHash, CaseInsensitiveEqual> memfiles;
    // handle -> (original path, size)
    std::unordered_map<HANDLE, std::pair<std::string, size_t>> memfile_handle_map;
#endif
#if WINVFS_ASAR
    std::list<asar::Archive*> asar_archives;
    std::unordered_map<std::string, std::pair<asar::FileEntry, asar::Archive*>, CaseInsensitiveHash, CaseInsensitiveEqual> asar_files;
    std::unordered_map<HANDLE, std::pair<asar::FileEntry, asar::Archive*>> asar_handle_map;
#endif
};

VFS& GetGlobalVFS();
