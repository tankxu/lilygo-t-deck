#!/bin/bash
# 按变体构建。
#
#   tools/build.sh            # daily(默认)
#   tools/build.sh car        # 车载 / Apple Music 控制器
#   tools/build.sh car clean  # 先 fullclean 再构建
#
# 两个变体各有独立的 build 目录和 sdkconfig —— 共用一套会来回触发全量重编,
# 而且 sdkconfig 里 BT 的开关会互相覆盖(BT 相关的几十个子符号一旦被 confgen
# 固化进 sdkconfig,关掉父开关也不会跟着撤销,踩过)。
#
#   daily → build/      sdkconfig
#   car   → build.car/  sdkconfig.car
#
# ⚠️ 按仓库约定走断网沙箱(见 /Users/Shared/LocalDev/CLAUDE.md):
# idf.py 会执行第三方构建脚本,不直接裸跑。
set -euo pipefail

VARIANT=${1:-daily}
shift || true

case "$VARIANT" in
  daily)
    BUILD_DIR=build
    SDKCONFIG=sdkconfig
    DEFAULTS="sdkconfig.defaults"
    ;;
  car)
    BUILD_DIR=build.car
    SDKCONFIG=sdkconfig.car
    DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.car"
    ;;
  *)
    echo "变体只能是 daily 或 car,收到:$VARIANT" >&2
    exit 1
    ;;
esac

cd "$(dirname "$0")/.."

if [ "${1:-}" = "clean" ]; then
    echo "==> 清掉 $BUILD_DIR 和 $SDKCONFIG"
    rm -rf "$BUILD_DIR" "$SDKCONFIG"
fi

# ⚠️ 变体必须走【环境变量】,不能用 -D。
# ESP-IDF 收集组件 REQUIRES 是在另一个 CMake 进程里跑的,-D 传不进去,
# 结果是 SRCS 按 car 编、REQUIRES 按 daily 解,nimble 头文件找不到。
# 详见 main/CMakeLists.txt 的说明。
export TDECK_VARIANT="$VARIANT"

ARGS=(
    -B "$BUILD_DIR"
    -D SDKCONFIG="$SDKCONFIG"
    -D SDKCONFIG_DEFAULTS="$DEFAULTS"
)

echo "==> 构建变体 $VARIANT → $BUILD_DIR"
safe-build -- idf.py "${ARGS[@]}" build
