#!/bin/bash

# macOS 应用签名、打包、公证完整流程脚本
# 用法: ./scripts/sign_and_package.sh [arm64|x86_64] [app_path]

set -e

# 检测架构参数
ARCH="${1:-$(uname -m)}"

# 标准化架构名称
case "$ARCH" in
    arm64|aarch64)
        ARCH="arm64"
        ;;
    x86_64|x86-64|amd64)
        ARCH="x86_64"
        ;;
    *)
        echo "错误: 不支持的架构 $ARCH"
        echo "用法: $0 [arm64|x86_64] [app_path]"
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/gitlab_build/$ARCH"
APP_NAME="Snapmaker_Orca"
APP_NAME_EX="Snapmaker Orca"
DMG_NAME="Snapmaker_Orca_${ARCH}.dmg"

# 证书配置
CERTIFICATE_ID="Developer ID Application: Shenzhen Snapmaker Technologies Co., Ltd. (5NGD3B3V37)"
ENTITLEMENTS="$PROJECT_DIR/scripts/disable_validation.entitlements"

# ============================================
# 公证凭据配置（已设置）
# ============================================
NOTARY_APPLE_ID="snapmaker-app@snapmaker.com"
NOTARY_TEAM_ID="5NGD3B3V37"
#NOTARY_KEYCHAIN_PROFILE="snapmaker"
NOTARY_PASSWORD="guhi-nuxy-mgnh-cbqs"

echo "=========================================="
echo "macOS 应用签名、打包、公证完整流程"
echo "=========================================="
echo "架构: $ARCH"
echo "证书: $CERTIFICATE_ID"
echo "TEAM_ID: 5NGD3B3V37"
echo "项目目录: $PROJECT_DIR"
echo

# ============================================
# 查找应用
# ============================================

# 如果提供了 app 路径
if [ -n "$2" ]; then
    SOURCE_APP="$2"
    if [ ! -d "$SOURCE_APP" ]; then
        echo "错误: 找不到应用: $SOURCE_APP"
        exit 1
    fi
    echo "使用指定应用: $SOURCE_APP"
else
    # 自动查找编译好的 app
    for possible_path in \
        "$BUILD_DIR/src/Release/$APP_NAME.app" \
        "$BUILD_DIR/src/RelWithDebInfo/$APP_NAME.app" \
        "$BUILD_DIR/src/$APP_NAME.app" \
        "$BUILD_DIR/src/Debug/$APP_NAME.app"
    do
        if [ -d "$possible_path" ]; then
            SOURCE_APP="$possible_path"
            echo "找到应用: $SOURCE_APP"
            break
        fi
    done

    # 检查空格版本名称
    if [ -z "$SOURCE_APP" ]; then
        for possible_path in \
            "$BUILD_DIR/src/Release/Snapmaker Orca.app" \
            "$BUILD_DIR/Snapmaker_Orca/Snapmaker Orca.app"
        do
            if [ -d "$possible_path" ]; then
                SOURCE_APP="$possible_path"
                echo "找到应用: $SOURCE_APP"
                break
            fi
        done
    fi

    if [ -z "$SOURCE_APP" ]; then
        echo "错误: 在 $BUILD_DIR 中找不到编译好的 $APP_NAME.app"
        echo "请先编译 $ARCH 版本: ./build_release_macos.sh -s -a $ARCH"
        exit 1
    fi
fi

# 创建临时工作目录
WORK_DIR="$BUILD_DIR/sign_package"
STAGING_DIR="$WORK_DIR/staging"
rm -rf "$WORK_DIR"
mkdir -p "$STAGING_DIR"

# ============================================
# 通用清理函数：彻底清除 Snapmaker 残留卷/目录
# 三种检测方式覆盖：hdiutil / diskutil / 文件系统目录
# ============================================
cleanup_volumes() {
    local pattern="${1:-Snapmaker}"

    # 方式1: hdiutil 管理的磁盘镜像
    hdiutil info 2>/dev/null | grep -oE "/Volumes/${pattern}[^\"]+" | sort -u | while read -r mp; do
        [ -d "$mp" ] || continue
        echo "  [hdiutil] 卸载: $mp"
        hdiutil detach "$mp" -force 2>/dev/null || true
    done

    # 方式2: 文件系统中的任何残留目录（含非 hdiutil 挂载、中断残留）
    local glob_pattern="/Volumes/${pattern}*"
    # 抑制 "No such file" 错误
    shopt -s nullglob 2>/dev/null || true
    for mp in $glob_pattern; do
        [ -d "$mp" ] || continue
        echo "  [fs] 清理: $mp"
        # 先尝试 diskutil 卸载
        diskutil unmount force "$mp" 2>/dev/null || true
        # 再尝试 hdiutil 卸载
        hdiutil detach "$mp" -force 2>/dev/null || true
        # 最后尝试删除残留空目录
        rmdir "$mp" 2>/dev/null || true
    done
    shopt -u nullglob 2>/dev/null || true

    sleep 2
    # 清空 mount 缓存
    rm -rf ~/Library/Caches/com.apple.dt.Xcode/Volumes 2>/dev/null || true
}

# 清理所有可能的残留挂载点（在开始工作前）
echo "清理可能的残留挂载点..."
cleanup_volumes

# 复制应用到工作目录
echo
echo "=========================================="
echo "步骤 1/6: 复制应用"
echo "=========================================="
echo "复制应用到工作目录..."
cp -R "$SOURCE_APP" "$STAGING_DIR/$APP_NAME.app"
FINAL_APP="$STAGING_DIR/$APP_NAME.app"

# 删除 .DS_Store 文件
find "$FINAL_APP" -name '.DS_Store' -delete
# 删除 PkgInfo 文件（冗余文件）
rm -f "$FINAL_APP/Contents/PkgInfo" 2>/dev/null || true

# 清理所有扩展属性（包括 com.apple.quarantine），避免 Gatekeeper 问题
echo "清理扩展属性..."
xattr -cr "$FINAL_APP" 2>/dev/null || {
    echo "  部分文件无法清除 xattr（可忽略）"
    find "$FINAL_APP" -type f -exec sh -c 'xattr -c "$1" 2>/dev/null || true' _ {} \;
}

# ============================================
# 打包外部依赖库
# ============================================

APP_MACOS_DIR="$FINAL_APP/Contents/MacOS"
APP_FRAMEWORKS_DIR="$FINAL_APP/Contents/Frameworks"
EXECUTABLE="$APP_MACOS_DIR/$APP_NAME"

# 确保 Frameworks 目录存在
mkdir -p "$APP_FRAMEWORKS_DIR"

echo
echo "检查并打包外部依赖库..."

# 查找所有外部依赖（非系统库）
EXTERNAL_LIBS=$(otool -L "$EXECUTABLE" | grep -E "opt/homebrew|usr/local|opt/local" | awk '{print $1}')

if [ -n "$EXTERNAL_LIBS" ]; then
    echo "发现外部依赖:"
    echo "$EXTERNAL_LIBS"
    echo

    for LIB_PATH in $EXTERNAL_LIBS; do
        if [ -f "$LIB_PATH" ]; then
            LIB_NAME=$(basename "$LIB_PATH")
            echo "处理: $LIB_NAME"

            # 获取实际的库文件路径（处理符号链接）- macOS 兼容方式
            if command -v realpath &> /dev/null; then
                REAL_LIB=$(realpath "$LIB_PATH" 2>/dev/null || echo "$LIB_PATH")
            else
                # macOS 不支持 realpath/readlink -f，使用 perl
                REAL_LIB=$(perl -MCwd=abs_path -e 'print abs_path(shift)' "$LIB_PATH" 2>/dev/null || echo "$LIB_PATH")
            fi
            REAL_NAME=$(basename "$REAL_LIB")

            # 复制实际的库文件
            if [ ! -f "$APP_FRAMEWORKS_DIR/$REAL_NAME" ]; then
                cp "$REAL_LIB" "$APP_FRAMEWORKS_DIR/$REAL_NAME"

                # 修改库的 ID 为文件名（不带路径）
                install_name_tool -id "$REAL_NAME" "$APP_FRAMEWORKS_DIR/$REAL_NAME"

                # 删除库中的 rpath（避免问题）
                install_name_tool -delete_rpath "@loader_path/../lib" "$APP_FRAMEWORKS_DIR/$REAL_NAME" 2>/dev/null || true
                install_name_tool -delete_rpath "@loader_path/lib" "$APP_FRAMEWORKS_DIR/$REAL_NAME" 2>/dev/null || true
            fi

            # 注释掉：不创建中间符号链接，避免冗余
            # if [ "$LIB_NAME" != "$REAL_NAME" ]; then
            #     (cd "$APP_FRAMEWORKS_DIR" && ln -sf "$REAL_NAME" "$LIB_NAME")
            # fi

            # 更新可执行文件中的依赖引用
            install_name_tool -change "$LIB_PATH" "@executable_path/../Frameworks/$REAL_NAME" "$EXECUTABLE" 2>/dev/null || true
            install_name_tool -change "$REAL_LIB" "@executable_path/../Frameworks/$REAL_NAME" "$EXECUTABLE" 2>/dev/null || true
        fi
    done

    echo
    echo "已打包的依赖库:"
    ls -la "$APP_FRAMEWORKS_DIR/"
else
    echo "没有外部依赖需要处理"
fi

# 移除不需要的 rpath
echo
echo "清理 rpath..."
install_name_tool -delete_rpath "/opt/homebrew/lib" "$EXECUTABLE" 2>/dev/null || true
install_name_tool -delete_rpath "/usr/local/lib" "$EXECUTABLE" 2>/dev/null || true
install_name_tool -delete_rpath "/opt/local/lib" "$EXECUTABLE" 2>/dev/null || true

# 修复 Resources 符号链接 (如果是符号链接)
RESOURCES_LINK="$FINAL_APP/Contents/Resources"
if [ -L "$RESOURCES_LINK" ]; then
    echo "修复 Resources 符号链接..."
    RESOURCES_TARGET=$(readlink "$RESOURCES_LINK")
    rm "$RESOURCES_LINK"
    cp -R "$RESOURCES_TARGET" "$RESOURCES_LINK"
fi

# 验证依赖
echo
echo "验证最终依赖:"
otool -L "$EXECUTABLE" | grep -E "@executable|libzstd|libsentry" || echo "无特殊依赖"

# ============================================
# 步骤 2/6: 签名应用
# ============================================

echo
echo "=========================================="
echo "步骤 2/6: 签名应用"
echo "=========================================="

APP_FRAMEWORKS_DIR="$FINAL_APP/Contents/Frameworks"
APP_MACOS_DIR="$FINAL_APP/Contents/MacOS"
EXECUTABLE="$APP_MACOS_DIR/$APP_NAME"

# 2.1 移除现有签名
echo "2.1 移除现有签名..."
codesign --remove-signature "$FINAL_APP" 2>/dev/null || true

# 2.2 签名 Frameworks 和动态库（使用 runtime 选项）
echo "2.2 签名 Frameworks 和动态库（使用 runtime 选项）..."
if [ -d "$APP_FRAMEWORKS_DIR" ]; then
    # 签名所有 .framework
    for framework in "$APP_FRAMEWORKS_DIR"/*.framework; do
        if [ -d "$framework" ]; then
            echo "  - 签名: $(basename "$framework")"
            codesign --force --verbose --options runtime --timestamp --sign "$CERTIFICATE_ID" "$framework" 2>/dev/null || true
        fi
    done

    # 签名所有 .dylib
    for dylib in "$APP_FRAMEWORKS_DIR"/*.dylib; do
        if [ -f "$dylib" ]; then
            echo "  - 签名: $(basename "$dylib")"
            codesign --force --verbose --options runtime --timestamp --sign "$CERTIFICATE_ID" "$dylib"
        fi
    done

    # 签名其他可能存在的库文件（如 .so）
    for lib in "$APP_FRAMEWORKS_DIR"/*.*; do
        if [ -f "$lib" ]; then
            case "$lib" in
                *.dylib) ;;  # 已处理，跳过
                *)
                    echo "  - 签名: $(basename "$lib")"
                    codesign --force --verbose --options runtime --timestamp --sign "$CERTIFICATE_ID" "$lib"
                    ;;
            esac
        fi
    done
fi

# 2.3 签名辅助工具
echo "2.3 签名辅助工具（使用 runtime 选项）..."
if [ -f "$APP_MACOS_DIR/crashpad_handler" ]; then
    echo "  - 签名: crashpad_handler"
    codesign --force --verbose --options runtime --timestamp --sign "$CERTIFICATE_ID" "$APP_MACOS_DIR/crashpad_handler"
fi

# 2.4 签名整个 app bundle（应用 entitlements）
echo "2.4 签名整个 app bundle（应用 entitlements）..."
echo "  这会签名所有组件并将 entitlements 应用到主可执行文件"
codesign --force --verbose --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" \
    --sign "$CERTIFICATE_ID" \
    "$FINAL_APP"

# 2.5 验证签名和 entitlements
echo "2.5 验证签名和 entitlements..."
echo "  检查签名..."
codesign -vvv "$FINAL_APP" 2>&1 | grep -E "valid on disk|Authority|TeamIdentifier" | head -5
echo ""
echo "  检查 entitlements..."
if codesign -d --entitlements - "$FINAL_APP" 2>&1 | grep -q "com.apple.security.cs.disable-library-validation"; then
    echo "  ✓ Entitlements 正确嵌入！"
else
    echo "警告: 预期的 entitlements 未找到"
fi

# ============================================
# 步骤 3/6: 创建并签名 DMG
# 两步法：创建空白镜像 -> 挂载到自定义路径 -> 复制内容 -> 卸载 -> 转换 UDZO
# 不使用 -srcfolder，因为 macOS 26.2+ 存在 bug：当卷名和内容条目名
# 有公共前缀时（Snapmaker_Orca vs Snapmaker Orca.app），任何到
# /Volumes/Snapmaker_Orca/Snapmaker Orca.app 的文件操作都被安全机制拦截。
# 将临时镜像挂载到非 /Volumes/ 的自定义路径可完全绕过此问题。
# ============================================

echo
echo "=========================================="
echo "步骤 3/6: 创建并签名 DMG"
echo "=========================================="

DMG_CONTENT_DIR="$WORK_DIR/dmg_content"
rm -rf "$DMG_CONTENT_DIR"
mkdir -p "$DMG_CONTENT_DIR"
rm -rf "$DMG_CONTENT_DIR/.fseventsd" 2>/dev/null || true

# 复制应用（显示名 Snapmaker Orca.app）并创建 Applications 符号链接
echo "准备 DMG 内容..."
cp -R "$FINAL_APP" "$DMG_CONTENT_DIR/$APP_NAME_EX.app"
xattr -cr "$DMG_CONTENT_DIR/$APP_NAME_EX.app" 2>/dev/null || {
    echo "  部分文件无法清除 xattr（可忽略）"
    find "$DMG_CONTENT_DIR/$APP_NAME_EX.app" -type f -exec sh -c 'xattr -c "$1" 2>/dev/null || true' _ {} \;
}
ln -sfn /Applications "$DMG_CONTENT_DIR/Applications"

DMG_VOLNAME="Snapmaker_Orca"
FINAL_DMG_PATH="$BUILD_DIR/$DMG_NAME"
rm -f "$FINAL_DMG_PATH"

# 计算镜像大小
APP_SIZE_KB=$(du -sk "$DMG_CONTENT_DIR" | cut -f1)
IMAGE_SIZE_MB=$((APP_SIZE_KB / 1024 + 200))
echo "DMG 内容大小: ${APP_SIZE_KB}KB, 镜像预留: ${IMAGE_SIZE_MB}MB"

# Step 1: 创建空白可写镜像（UDIF，JHFS+）
echo "创建空白镜像..."
TMP_IMG="$WORK_DIR/tmp_dmg.dmg"
hdiutil create \
    -size ${IMAGE_SIZE_MB}m \
    -volname "$DMG_VOLNAME" \
    -layout SPUD \
    -fs JHFS+ \
    -type UDIF \
    -ov \
    -o "$TMP_IMG" || {
    echo "错误: 创建空白镜像失败"
    exit 1
}

# Step 2: 挂载到自定义路径（非 /Volumes/，绕过 macOS 26.2 安全限制）
echo "挂载镜像到自定义路径..."
CUSTOM_MOUNT="$WORK_DIR/dmg_mount"
rm -rf "$CUSTOM_MOUNT"
mkdir -p "$CUSTOM_MOUNT"
hdiutil attach "$TMP_IMG" \
    -noverify \
    -noautoopen \
    -mountpoint "$CUSTOM_MOUNT" || {
    echo "错误: 挂载镜像失败"
    rm -f "$TMP_IMG"
    exit 1
}

# Step 3: 复制内容到镜像
echo "复制内容到镜像..."
cp -Rp "$DMG_CONTENT_DIR"/* "$CUSTOM_MOUNT/" || {
    echo "错误: 复制内容失败"
    hdiutil detach "$CUSTOM_MOUNT" -force 2>/dev/null || true
    rm -f "$TMP_IMG"
    exit 1
}
echo "  已复制: $(du -sh "$CUSTOM_MOUNT" | cut -f1)"

# Step 4: 卸载镜像
echo "卸载镜像..."
hdiutil detach "$CUSTOM_MOUNT" || {
    echo "警告: 正常卸载失败，尝试强制卸载"
    hdiutil detach "$CUSTOM_MOUNT" -force 2>/dev/null || true
}
rmdir "$CUSTOM_MOUNT" 2>/dev/null || true

# Step 5: 转换为压缩 UDZO
echo "压缩 DMG (UDZO)..."
hdiutil convert "$TMP_IMG" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "$FINAL_DMG_PATH" || {
    echo "错误: DMG 格式转换失败"
    rm -f "$TMP_IMG" "$TMP_IMG."*.dmg 2>/dev/null || true
    exit 1
}

# 清理临时镜像
rm -f "$TMP_IMG" "$TMP_IMG."*.dmg 2>/dev/null || true

[ ! -f "$FINAL_DMG_PATH" ] && echo "错误: 未生成 DMG" && exit 1

# 签名 DMG
echo "签名 DMG..."
codesign --force --timestamp --sign "$CERTIFICATE_ID" "$FINAL_DMG_PATH"

echo "验证 DMG 签名..."
codesign -vvv "$FINAL_DMG_PATH" 2>&1 | head -3

rm -rf "$DMG_CONTENT_DIR"

echo ""
echo "=========================================="
echo "DMG 创建和签名完成!"
echo "=========================================="
echo "DMG: $FINAL_DMG_PATH"
echo "大小: $(du -h "$FINAL_DMG_PATH" | cut -f1)"

# ============================================
# 步骤 4/6: 公证 DMG
# ============================================

echo ""
echo "=========================================="
echo "步骤 4/6: 公证 DMG"
echo "=========================================="

# 判断是否可公证：检查密码是否已设置
echo "检查公证凭据..."
echo "  Apple ID: $NOTARY_APPLE_ID"
echo "  Team ID: $NOTARY_TEAM_ID"

if [ -z "$NOTARY_PASSWORD" ] || [ "$NOTARY_PASSWORD" = "__PLEASE_ENTER_PASSWORD__" ]; then
    echo ""
    echo "密码未设置！"
    echo ""
    echo "请在脚本中设置密码："
    echo "  NOTARY_PASSWORD=\"your-app-specific-password\""
    echo ""
    echo "或者通过环境变量设置："
    echo "  export NOTARY_PASSWORD=\"your-app-specific-password\""
    echo ""
    echo "跳过公证步骤..."
else
    echo "✓ 密码已配置"
    echo ""
    echo "=========================================="
    echo "步骤 5/6: 提交公证"
    echo "=========================================="

    echo "提交 DMG 到 Apple 公证服务..."
    xcrun notarytool submit "$FINAL_DMG_PATH" \
        --apple-id "$NOTARY_APPLE_ID" \
        --team-id "$NOTARY_TEAM_ID" \
        --password "$NOTARY_PASSWORD" \
        --wait \
        --progress

    echo ""
    echo "=========================================="
    echo "步骤 6/6: 装订公证票据"
    echo "=========================================="

    echo "装订公证票据到 DMG..."
    xcrun stapler staple "$FINAL_DMG_PATH"

    # 验证公证结果
    echo ""
    echo "验证公证结果..."
    xcrun stapler validate -v "$FINAL_DMG_PATH"

    echo ""
    echo "=========================================="
    echo "公证完成!"
    echo "=========================================="
    echo "此 DMG 已签名并公证，可以在任何 Mac 上无缝运行"
fi

echo ""
echo "=========================================="
echo "完成!"
echo "=========================================="
echo "架构: $ARCH"
echo "应用: $FINAL_APP"
echo "DMG: $FINAL_DMG_PATH"
echo "证书: $CERTIFICATE_ID"
echo "TEAM_ID: 5NGD3B3V37"
echo ""
echo "使用方法:"
echo "  1. 打开 DMG: open $FINAL_DMG_PATH"
echo "  2. 将 $APP_NAME_EX.app 拖拽到 Applications 文件夹"
echo "  3. 从 Applications 运行应用"
echo "=========================================="
