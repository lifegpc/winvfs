project('winvfs', default_options : ['cpp_std=c++17'])
add_languages('c', 'cpp', required: true)

zlib = subproject('zlib', required: true)
utils = subproject('utils', required: true)
detours = subproject('detours', required: true)
if get_option('zstd')
    zstd = subproject('zstd', required: true)
    xp3vfs = subproject('xp3vfs', required: true, default_options : ['zstd=true'])
else
    xp3vfs = subproject('xp3vfs', required: true, default_options : ['zstd=false'])
endif

zlib_dep = zlib.get_variable('zlib_dep')
utils_dep = utils.get_variable('utils_dep')
detours_dep = detours.get_variable('detours_dep')
xp3vfs_dep = xp3vfs.get_variable('xp3vfs_dep')

deps = [
    zlib_dep,
    utils_dep,
    xp3vfs_dep,
    detours_dep,
]

cc = meson.get_compiler('c')

MSVC = cc.get_id() == 'msvc'
if MSVC
    add_project_arguments('/utf-8', language: 'c')
    add_project_arguments('/utf-8', language: 'cpp')
endif

winvfs = static_library('winvfs',
    sources : files('winvfs.cpp', 'winvfs.hpp'),
    dependencies : deps,
)

winvfs_dep = declare_dependency(
    link_with : winvfs,
    dependencies : deps,
    include_directories : include_directories('.'),
)

default_test = false
if not meson.is_subproject()
    default_test = true
endif

if get_option('build_tests').enable_auto_if(default_test).enabled()
    executable('winvfs_test', sources: ['winvfs_test.cpp'], dependencies: [winvfs_dep])
endif
