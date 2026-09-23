#!/usr/bin/env python3
"""生成 320x240 RGB565 壁纸 —— domain-warped fBm。

做法沿用 Inigo Quilez 的 warped fBm(就是 macOS Sonoma / iOS 那类流动色场的
底层套路):不直接用噪声当颜色,而是【用噪声去扭曲采样坐标】,扭两层之后再取值。
一层扭曲得到的是普通的云纹,两层才会出现那种被拉丝、有流向的有机质感。

    q = ( fbm(p),          fbm(p + a) )
    r = ( fbm(p + 4q + b), fbm(p + 4q + c) )
    f =   fbm(p + 4r)

颜色不只按 f 取,还用 q.x / r.y 去调制 —— 这是让画面出现"层次"而不是
单纯明暗变化的关键:不同区域的主色相会被不同的中间量推开。

最后两步都不能省:
  提亮  向白色插值 45%。浅色底才压得住背光漏光(见 docs/hardware.md),
        也让白色卡片浮得起来。
  抖动  RGB565 每通道只有 5/6 位,如此平滑的渐变直接量化必然出现色带,
        在 320x240 上非常显眼。量化前加三角分布噪声把误差打散,
        色带变成肉眼分不出的颗粒。
"""
import math, struct, pathlib, random

W, H = 320, 240
SEED = 1337

# ── 值噪声 ────────────────────────────────────────────────
random.seed(SEED)
PERM = list(range(256))
random.shuffle(PERM)
PERM = PERM * 2
GRAD = [random.random() for _ in range(512)]

def fade(t):            # 五次平滑,一阶二阶导都连续,不会出现网格痕迹
    return t * t * t * (t * (t * 6 - 15) + 10)

def vnoise(x, y):
    xi, yi = int(math.floor(x)), int(math.floor(y))
    xf, yf = x - xi, y - yi
    u, v = fade(xf), fade(yf)
    xi &= 255; yi &= 255
    a = GRAD[(PERM[xi]     + yi)     & 511]
    b = GRAD[(PERM[xi + 1] + yi)     & 511]
    c = GRAD[(PERM[xi]     + yi + 1) & 511]
    d = GRAD[(PERM[xi + 1] + yi + 1) & 511]
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v

def fbm(x, y, oct=3):
    s, amp, freq = 0.0, 0.5, 1.0
    for _ in range(oct):
        s += amp * vnoise(x * freq, y * freq)
        freq *= 2.0
        amp  *= 0.5
    return s

# ── 调色板 ────────────────────────────────────────────────
# 军绿系。外壳是透明军绿耗材打印的,UI 跟着走,机器才像一件完整的东西。
#
# 做法是【单一色带】而不是多层 mix:之前叠三层 mix,每层都往浅色拉,
# 叠完整张图糊成一片。现在 f 归一化后直接在四个色标之间插值,
# q/r 只做 ±15% 的微调 —— 层次感来自色带本身,不是来自叠加。
STOPS = [
    (0.00, (0.22, 0.31, 0.18)),   # 深橄榄(阴影)
    (0.36, (0.46, 0.60, 0.34)),   # 苔藓绿
    (0.64, (0.78, 0.81, 0.56)),   # 卡其
    (1.00, (0.97, 0.96, 0.87)),   # 暖米(高光)
]

def ramp(t):
    t = min(1.0, max(0.0, t))
    for i in range(len(STOPS) - 1):
        a, ca = STOPS[i]
        b, cb = STOPS[i + 1]
        if t <= b:
            k = (t - a) / (b - a)
            return tuple(ca[j] + (cb[j] - ca[j]) * k for j in range(3))
    return STOPS[-1][1]

# fbm 三倍频的输出大致落在 [0.18, 0.68],归一化到色带的 0..1
F_LO, F_HI = 0.18, 0.68

LIGHTEN = 0.34
TOP_LIFT = 0.20
EDGE_LIFT = 0.62   # 反向暗角:越靠边越亮
SCALE   = 1.35
WARP    = 2.6

# 抖动幅度必须和【量化步长】同量级,不是 1 个 8bit LSB。
# RGB565:R/B 占 5 位 → 步长 255/31 ≈ 8.2;G 占 6 位 → 255/63 ≈ 4.05。
DITHER_RB = 255.0 / 31.0 / 2.0
DITHER_G  = 255.0 / 63.0 / 2.0

out = bytearray()
rnd = random.Random(SEED ^ 0x5A5A)
for py in range(H):
    y = py / H * SCALE
    row = bytearray()
    for px in range(W):
        x = px / W * SCALE * (W / H)     # 按宽高比拉伸,避免横向被压扁

        qx = fbm(x + 2.1, y + 6.7)
        qy = fbm(x + 5.2, y + 1.3)
        rx = fbm(x + WARP * qx + 1.7, y + WARP * qy + 9.2)
        ry = fbm(x + WARP * qx + 8.3, y + WARP * qy + 2.8)
        f  = fbm(x + WARP * rx, y + WARP * ry)

        fn  = (f - F_LO) / (F_HI - F_LO)
        fn += (qx - 0.45) * 0.22 + (ry - 0.45) * 0.14   # 微调,制造流向而不改整体调子
        col = ramp(fn)

        r, g, b = (c * 255.0 for c in col)
        # 反向暗角(vignette 的反面):越靠画面边缘越亮。
        # 常规暗角是四角压暗,这里必须反过来 —— 这块屏的背光漏光集中在
        # 右上/左下/右下三个角(见 docs/hardware.md),角落一暗,漏出来的红光
        # 立刻就显形了。主动把四角提亮,漏光就被压在底下看不见。
        nx = (px / (W - 1)) * 2.0 - 1.0
        ny = (py / (H - 1)) * 2.0 - 1.0
        # 用 ^1.5 而不是线性:中心区域几乎不受影响,只在最外圈猛提 ——
        # 漏光只在四角,没必要把整张图洗白
        edge = min(1.0, (nx * nx + ny * ny) / 2.0) ** 1.5
        lift = LIGHTEN + TOP_LIFT * (1.0 - py / H) + EDGE_LIFT * edge
        r += (255 - r) * lift
        g += (255 - g) * lift
        b += (255 - b) * lift

        def dz(amp):
            return (rnd.random() - rnd.random()) * amp
        r = min(255.0, max(0.0, r + dz(DITHER_RB)))
        g = min(255.0, max(0.0, g + dz(DITHER_G)))
        b = min(255.0, max(0.0, b + dz(DITHER_RB)))

        # 四舍五入到各通道的量化格点(不是直接截断 —— 截断本身就会引入半格偏移)
        val = ((int(r * 31 / 255 + 0.5) & 0x1F) << 11) \
            | ((int(g * 63 / 255 + 0.5) & 0x3F) << 5)  \
            |  (int(b * 31 / 255 + 0.5) & 0x1F)
        row += struct.pack('<H', val)
    out += row

p = pathlib.Path(__file__).resolve().parent.parent / "main/assets/wallpaper.rgb565"
p.write_bytes(out)
print(f"{p}  {len(out)} 字节  ({W}x{H} RGB565, domain-warped fBm, seed {SEED})")
