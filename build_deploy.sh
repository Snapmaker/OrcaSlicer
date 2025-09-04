#!/bin/zsh

# 1. 创建依赖目录
# mkdir -p deps/build_x86_64/OrcaSlicer_dep_x86_64

# 2. 构建依赖（使用 -d 参数）
# ./build_release_macos.sh -d -a x86_64 -t

# 3. 打包依赖（使用 -p 参数）
# ./build_release_macos.sh -d -p -a x86_64 -t

# 设置架构变量（根据需要修改）
# ARCH="x86_64"  # 或 "arm64"

# # 安装必要工具
# brew install tree ninja libtool

# # 列出已安装的包
# brew list

# # 创建依赖目录
# PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# mkdir -p "$PROJECT_DIR/deps/build_$ARCH"
# mkdir -p "$PROJECT_DIR/deps/build_$ARCH/OrcaSlicer_dep_$ARCH"

# # 显示目录结构
# # tree "$PROJECT_DIR/deps/build_$ARCH/OrcaSlicer_dep_$ARCH"

# # 复制依赖到依赖目录
# cp -r ./deps/build_x86_64/OrcaSlicer_dep_x86_64 "$PROJECT_DIR/deps/build_x86_64/OrcaSlicer_dep_x86_64"

# 使用 Ninja 构建器构建 Slicer
# ./build_release_macos.sh -s -n -x -a x86_64 -t

# 定义变量
PROJECT_ROOT=$(pwd)
ARCH="x86_64"
VER="2.1.0"           # 改为你的版本号
APP_NAME="Snapmaker_Orca"

# 创建符号链接
ln -sf /Applications "$PROJECT_ROOT/build_$ARCH/$APP_NAME/Applications"

# 生成 DMG
hdiutil create \
  -volname "$APP_NAME" \
  -srcfolder "$PROJECT_ROOT/build_$ARCH/$APP_NAME" \
  -ov \
  -format UDZO \
  "${APP_NAME}_Mac_${ARCH}_${VER}.dmg"

# 清理符号链接（可选）
rm "$PROJECT_ROOT/build_$ARCH/$APP_NAME/Applications"