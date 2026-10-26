# deps/mesa — MESA llvmpipe 软件渲染回退

## 用途

Windows 上当硬件 OpenGL 驱动不可用(显卡被禁用 / 驱动缺失 / Microsoft 基本显示适配器,
仅提供 OpenGL 1.1)时,启动器壳 `snapmaker-orca.exe`
(`src/Snapmaker_Orca_app_msvc.cpp`)会在加载 `Snapmaker_Orca.dll` 之前自动加载本目录
`mesa\opengl32.dll`(Mesa llvmpipe,CPU 软件光栅化),使 3D 视口以软件渲染继续可用。

该 DLL 必须随安装包分发到 **安装根目录的 `mesa\` 子目录**(由
`src/CMakeLists.txt` 的 install 规则完成)。也可用命令行参数强制:

- `snapmaker-orca.exe --sw-renderer`  强制软件渲染
- `snapmaker-orca.exe --no-sw-renderer` 强制硬件渲染

## 当前版本

| 项 | 值 |
|---|---|
| 来源 | https://github.com/mmozeiko/build-mesa (release 26.2.1) |
| 资产 | `mesa-llvmpipe-x64-26.2.1.7z`(发布页附带官方 SHA256) |
| 7z SHA256 | `5c9a68b3d898a181c896163f345db4f79392429e2b2452373f797984bfa2e7a6` |
| 解压后 opengl32.dll SHA256 | `b463d727b4d187802b3a5b3c2498c0a1566996324b1ee41aa1335b438c7cfd0b` |
| 架构 | PE32+ x86-64,自包含(仅依赖系统 DLL,无需伴随 mesa 模块) |
| 许可 | MIT(应用内 About 对话框已含 Mesa 3D 致谢) |

升级时替换 `x64/opengl32.dll` 并更新上表校验值。来源项目持续跟踪 Mesa 上游,
提供 x86/x64/arm64 与 msvc/mingw 变体;若未来发布 arm64 安装包,取对应
`mesa-llvmpipe-arm64-*.7z` 放入 `arm64/` 子目录并同步修改 CMake install 规则。

## 注意

- 该 DLL 依赖 `api-ms-win-core-synch-l1-2-0.dll`(Win8.1+ API Set),
  与应用其余部分的 Win10+ 前提一致。
- 58.9 MB 原始体积,LZMA 压缩后约 16 MB(installer.nsi 即 lzma)。
- 二进制以普通 git 对象入库;如团队启用 Git LFS,建议把
  `deps/mesa/**/*.dll` 迁移到 LFS。
