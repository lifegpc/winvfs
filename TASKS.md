# 外部接口
- [x] `VFS::VFS()`: 构造函数，初始化虚拟文件系统。
- [x] `VFS::~VFS()`: 析构函数，清理资源。
- [x] `bool VFS::AddArchive(const char* path)`: 添加一个新的归档文件到虚拟文件系统中。
- [ ] `bool VFS::Init()`: 初始化Hook

# NTAPI Hook
- [x] NtCreateFile
- [x] NtReadFile
- [ ] NtWriteFile
- [x] NtClose
- [x] NtQueryInformationFile
- [x] NtQueryObject
