#!/bin/bash
# 排他占用 T-Deck(串口只有一个,烧录/monitor/HTTP 验证都得独占)。
#
# macOS 自带的 util-linux 里【没有 flock】,所以用 mkdir 做原子锁 ——
# mkdir 在同一个文件系统上是原子的,存在就失败,正好当互斥量。
#
#   tools/with-device.sh idf.py -p /dev/cu.usbmodem1101 flash
#
# 锁是自动释放的(EXIT trap)。进程被 kill -9 的话会留下死锁目录,
# 超过 15 分钟的锁会被下一个等待者判定为陈旧并抢走。
set -u
LOCK=/tmp/tdeck-device.lock.d
WAIT=${TDECK_LOCK_WAIT:-900}

for ((i = 0; i < WAIT; i++)); do
    if mkdir "$LOCK" 2>/dev/null; then
        echo "$$" > "$LOCK/pid"
        trap 'rm -rf "$LOCK"' EXIT
        exec "$@"
    fi
    # 陈旧锁回收:持有者已经不在了,或者拿着超过 15 分钟没放
    if [ -f "$LOCK/pid" ]; then
        holder=$(cat "$LOCK/pid" 2>/dev/null)
        if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
            echo "锁的持有者 $holder 已经没了,回收" >&2
            rm -rf "$LOCK"
            continue
        fi
    fi
    if [ -d "$LOCK" ] && [ -z "$(find "$LOCK" -maxdepth 0 -mmin -15 2>/dev/null)" ]; then
        echo "锁超过 15 分钟没释放,判定为陈旧,回收" >&2
        rm -rf "$LOCK"
        continue
    fi
    sleep 1
done
echo "等锁超时($WAIT 秒),设备被别人占着" >&2
exit 1
