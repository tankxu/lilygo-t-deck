#!/bin/bash
# 通过 WiFi 把某个变体的固件推到板子上。
#
#   tools/ota.sh                  # 推 daily
#   tools/ota.sh car              # 推 car
#   tools/ota.sh car 192.168.1.42
#   tools/ota.sh 192.168.1.42     # 省略变体时第一个参数也可以直接写 IP
#
# 写的是另一个 OTA 槽,正在跑的那个不动 —— 推挂了最坏就是白推一次。
# 新固件起来后要自己声明可用(见 debug_server.cc),否则 bootloader 自动回滚。
#
# ⚠️ 两个槽现在各装一个【变体】(见 main/sys/variant.h)。推送永远写
# "当前没在跑的那个槽",也就是另一个变体所在的槽 —— 所以要更新日常固件,
# 得先切到车载模式再推,反之亦然。推之前脚本会告诉你现在跑的是哪个。
set -e

VARIANT=daily
case "${1:-}" in
    daily|car) VARIANT=$1; shift ;;
esac

IP=${1:-${TDECK_IP:-192.168.1.136}}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

case "$VARIANT" in
    daily) BIN="$ROOT/build/tdeck-os.bin" ;;
    car)   BIN="$ROOT/build.car/tdeck-os-car.bin" ;;
esac
[ -f "$BIN" ] || { echo "没有 $BIN,先 tools/build.sh $VARIANT"; exit 1; }

# ── 防覆盖 ──────────────────────────────────────────────
# 推送永远写"当前没在跑的那个槽"。两个槽各装一个变体的时候,这意味着:
# 只有设备正跑着【另一个】变体时,推送才是安全的;
# 跑着 X 又推 X,写的就是另一个变体所在的槽,把它覆盖掉。
#
# 这不是假想 —— 实际踩过一次:切换那一步没生效就直接推了,
# 结果两个槽都变成车载,日常固件没了,而且当时毫无提示。
RUNNING=$(curl -s --max-time 5 "http://$IP/info" | sed -n 's/.*"variant":"\([^"]*\)".*/\1/p')
case "$RUNNING" in
    tdeck-os-car) RUNNING_V=car ;;
    tdeck-os)     RUNNING_V=daily ;;
    *)            RUNNING_V="" ;;
esac
if [ -n "$RUNNING_V" ] && [ "$RUNNING_V" = "$VARIANT" ] && [ "${FORCE:-}" != "1" ]; then
    OTHER=$([ "$VARIANT" = car ] && echo daily || echo car)
    echo "拒绝推送:板子正跑着 $VARIANT,推 $VARIANT 会写到【$OTHER 所在的槽】,把它覆盖掉。"
    echo "  先在板子上切到 $OTHER(应用页的「固件变体」),再推。"
    echo "  确实要覆盖就 FORCE=1 tools/ota.sh $VARIANT"
    exit 1
fi

SZ=$(stat -f%z "$BIN")
# 镜像里 esp_app_desc 的 app_elf_sha256 字段:24 字节镜像头 + 8 字节段头 = 0x20,
# 再跳过 magic/secure_ver/reserv/version/project/time/date/idf_ver 共 144 字节 -> 0xB0。
# 拿它和板子 /info 报的 sha 对一下,就能确认"跑着的确实是我刚推的那份"。
WANT=$(xxd -p -s 176 -l 16 "$BIN")
echo "推 $VARIANT($(( SZ / 1024 )) KB)到 $IP  (sha $WANT)"
echo "  推之前:$(curl -s --max-time 5 "http://$IP/info" || true)"

curl -f --max-time 300 --progress-bar \
     -H 'Content-Type: application/octet-stream' \
     --data-binary "@$BIN" "http://$IP/ota" || { echo "推送失败"; exit 1; }

echo "等板子重启..."
for i in $(seq 1 40); do
    sleep 3
    OUT=$(curl -s --max-time 3 "http://$IP/info" || true)
    case "$OUT" in
        *uptime_s*)
            echo "  起来了:$OUT"
            # ⚠️ 前缀比对,不能求相等 —— esp_app_get_elf_sha256 按缓冲区大小
            # 截断,板子报的是 SHA 的头几位(现在是 9 位),不是全长。
            GOT=$(echo "$OUT" | sed -n 's/.*"sha":"\([0-9a-f]*\)".*/\1/p')
            case "$WANT" in
                "$GOT"*) [ -n "$GOT" ] && echo "✅ sha 前缀对得上($GOT),跑的就是刚推的这份" ;;
                *) echo "⚠️  sha 对不上(板子 $GOT / 镜像 $WANT),可能回滚了"; exit 1 ;;
            esac
            exit 0 ;;
    esac
    printf '.'
done
echo
echo "⚠️  40 次都没连上。板子应该已经自动回滚到上一版了,插 USB 看串口。"
exit 1
