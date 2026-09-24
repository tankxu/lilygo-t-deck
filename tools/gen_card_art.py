#!/usr/bin/env python3
"""生成应用卡片的整幅背景图(143x94 RGB565)。

为什么卡片要用整幅图而不是画矢量:
    实测(LV_USE_PERF_MONITOR,滑动中)四张卡片的矢量内容 —— 均衡器条、
    仪表盘圆弧、色带、清单方块 —— 每帧要 75ms 重画一遍,而它们一动不动。
    换成整幅位图之后每帧只是贴图。好看和流畅在这里是同一件事。

手法沿用 tools/gen_wallpaper.py 的 domain-warped fBm(Inigo Quilez 那套):
用噪声去扭曲采样坐标,扭两层再取值,得到有流向的有机色场,而不是死板的
线性渐变。四张图共用同一套扭曲参数、只换配色,这样它们看起来像一家人。

和壁纸的两点不同:
  · 壁纸要压背光漏光,所以向白色提亮 45%;卡片要的是【浓郁】,不提亮。
  · 左下角要放白色标题,所以往那儿压一层平滑的暗角 —— 注意是【渐变】,
    不是一条半透明黑条。白字压在渐变上才不显脏。

RGB565 每通道只有 5/6 位,这么平滑的色场直接量化必然起色带,
量化前加三角分布噪声把误差打散。
"""
import math, struct, pathlib, random

W, H = 143, 94
SEED = 20260925

# ── 值噪声 ────────────────────────────────────────────────
random.seed(SEED)
PERM = list(range(256))
random.shuffle(PERM)
PERM += PERM


def _fade(t):
    return t * t * (3 - 2 * t)


def _grad(h, x, y):
    # 8 个方向的梯度,够用且比查表快
    h &= 7
    u, v = (x, y) if h < 4 else (y, x)
    return (u if h & 1 == 0 else -u) + (v if h & 2 == 0 else -v)


def noise(x, y):
    xi, yi = int(math.floor(x)) & 255, int(math.floor(y)) & 255
    xf, yf = x - math.floor(x), y - math.floor(y)
    u, v = _fade(xf), _fade(yf)
    aa = PERM[PERM[xi] + yi]
    ab = PERM[PERM[xi] + yi + 1]
    ba = PERM[PERM[xi + 1] + yi]
    bb = PERM[PERM[xi + 1] + yi + 1]
    x1 = _grad(aa, xf, yf) + u * (_grad(ba, xf - 1, yf) - _grad(aa, xf, yf))
    x2 = _grad(ab, xf, yf - 1) + u * (_grad(bb, xf - 1, yf - 1) - _grad(ab, xf, yf - 1))
    return (x1 + v * (x2 - x1)) * 0.5 + 0.5


def fbm(x, y, oct=3):
    v, amp, freq = 0.0, 0.5, 1.0
    for _ in range(oct):
        v += amp * noise(x * freq, y * freq)
        freq *= 2.0
        amp *= 0.5
    return v


def warped(x, y):
    """返回 (f, qx, ry) —— 主值和两个中间量,用来调制色相。"""
    qx = fbm(x, y)
    qy = fbm(x + 5.2, y + 1.3)
    rx = fbm(x + 2.0 * qx + 1.7, y + 2.0 * qy + 9.2)
    ry = fbm(x + 2.0 * qx + 8.3, y + 2.0 * qy + 2.8)
    return fbm(x + 2.0 * rx, y + 2.0 * ry), qx, ry


# ── 配色 ──────────────────────────────────────────────────
# 每张卡一条多段色带。四条都走"深 → 饱和 → 亮"的同一种结构,
# 只是色相不同,所以并排放在桌面上是一套而不是四张不相干的图。
RAMPS = {
    # 音乐:紫罗兰 → 洋红 → 暖橙,最有"在放东西"的劲儿
    "music": [(0.00, (26, 18, 54)), (0.35, (86, 34, 120)),
              (0.62, (188, 52, 118)), (0.85, (240, 126, 74)), (1.00, (252, 196, 128))],
    # 设置:石板蓝 → 钢青 → 冰蓝,冷静、像仪器
    "settings": [(0.00, (22, 38, 60)), (0.34, (40, 84, 122)),
                 (0.62, (64, 140, 168)), (0.86, (134, 202, 210)), (1.00, (206, 238, 242))],
    # 调色板:整条光谱,它本来就是讲颜色的应用
    "palette": [(0.00, (48, 16, 72)), (0.22, (28, 72, 160)), (0.44, (32, 158, 132)),
                (0.62, (226, 194, 58)), (0.80, (226, 96, 52)), (1.00, (236, 158, 176))],
    # 自检:琥珀 → 橄榄 → 黄绿,和整机的军绿主题最近
    "selftest": [(0.00, (28, 26, 14)), (0.34, (86, 74, 26)),
                 (0.60, (142, 124, 40)), (0.84, (176, 168, 72)), (1.00, (222, 216, 150))],
}


def ramp_at(ramp, t):
    t = max(0.0, min(1.0, t))
    for i in range(len(ramp) - 1):
        t0, c0 = ramp[i]
        t1, c1 = ramp[i + 1]
        if t0 <= t <= t1:
            k = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
            k = _fade(k)                       # 段间也平滑,免得接缝处出现折角
            return tuple(c0[j] + (c1[j] - c0[j]) * k for j in range(3))
    return ramp[-1][1]


def tri_dither():
    """三角分布噪声:两个均匀随机相减。比单个均匀噪声更贴近理想抖动。"""
    return random.random() - random.random()


def gen(name, ramp, path):
    px = bytearray()
    for y in range(H):
        for x in range(W):
            nx, ny = x / W * 1.05, y / H * 0.72
            f, qx, ry = warped(nx, ny)

            # 主值决定在色带上的位置;两个中间量把不同区域推向不同色相,
            # 这是画面出现"层次"而不是单纯明暗变化的关键。
            # 斜向渐变打底(左上暗、右下亮),噪声只负责把它揉出有机感。
            # 纯噪声配色会得到一团均匀的云,没有构图。
            diag = (x / W) * 0.55 + (1.0 - y / H) * 0.45
            t = diag * 0.62 + f * 0.26 + qx * 0.08 + ry * 0.04
            t = (t - 0.20) / 0.60                      # 拉满动态范围
            r, g, b = ramp_at(ramp, t)

            # 左下暗角:给白色标题让路。横向到 62% 宽就衰减完,
            # 纵向只影响下面 42%,而且是平滑曲线 —— 不是一条黑条。
            fx = max(0.0, 1.0 - (x / W) / 0.55)
            fy = max(0.0, (y / H - 0.66) / 0.34)
            shade = 1.0 - 0.48 * _fade(min(1.0, fx * 0.45 + fy * 0.80) if fy > 0 else 0.0)
            r, g, b = r * shade, g * shade, b * shade

            # 四周极轻的暗角,让卡片边缘收得住
            dx, dy = (x / W - 0.5) * 2, (y / H - 0.5) * 2
            vig = 1.0 - 0.13 * min(1.0, (dx * dx + dy * dy) * 0.55)
            r, g, b = r * vig, g * vig, b * vig

            d = tri_dither()
            r5 = max(0, min(31, int(r / 255 * 31 + d * 0.5 + 0.5)))
            g6 = max(0, min(63, int(g / 255 * 63 + d * 0.5 + 0.5)))
            b5 = max(0, min(31, int(b / 255 * 31 + d * 0.5 + 0.5)))
            px += struct.pack("<H", (r5 << 11) | (g6 << 5) | b5)

    path.write_bytes(px)
    print(f"  {name:9s} {len(px):6d} 字节  {path}")


def main():
    out = pathlib.Path(__file__).resolve().parent.parent / "main" / "assets"
    out.mkdir(parents=True, exist_ok=True)
    print(f"生成卡片配图 {W}x{H} RGB565:")
    for name, ramp in RAMPS.items():
        random.seed(SEED + sum(ord(c) for c in name))   # 每张图的抖动不同相
        gen(name, ramp, out / f"card_{name}.rgb565")


if __name__ == "__main__":
    main()
