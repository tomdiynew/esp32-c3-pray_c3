# 祷告时光 esp32-c3-pray_c3 · 与耶稣一同祷告

ESP32-C3 SuperMini + 1.50 寸 240×280 SPI 彩屏（GC9306 / GC9307）做的祷告小程序，基于 **ESP-IDF v6.1**。
先默祷，再按一下板载 BOOT 键点亮一支祈祷蜡烛——按键不是祷告本身，只是祷告之后的点烛纪念，
和在教堂里点一支蜡烛一样。点亮的蜡烛数保存在 Flash 里。

![preview](art/preview.png)

预览图从左到右：开机画面 / 祷告场景（点烛前的提示）/ 进入场景时的「今日经文」。

## 玩法

| 画面 | 操作 | 效果 |
|---|---|---|
| 开机画面 | 单击 BOOT | 进入祷告场景，显示今日经文 |
| 祷告场景 | 默祷后按下 BOOT | 点亮一支蜡烛，按下即响应 |
| 祷告场景 | 长按 BOOT **1 秒** | 点烛记录清零，并从 Flash 删除，提示「点烛记录已清零」 |

- **开机**：耶稣合掌祷告的画面铺满全屏，光点缓缓上升；右下角是「与耶稣一同祷告 · 开始祷告」按钮。
  按钮放在右下角，是为了不遮挡祷告台上的金色十字架。
- **今日经文**：进入祷告场景时，柔和的光束从上方洒下，浮现一张羊皮纸卡片，随机显示一则圣经经文摘句
  （和合本）和出处，约 8 秒后淡出。共 30 则，见 [`art/verses.txt`](art/verses.txt)。
  经文不与祷告次数挂钩，不是「祷告满多少次的奖励」。
- **祷告场景**：
  - 画面：金色拱框里是祷告中的耶稣（从开机图裁出），两侧柔和的彩窗和光束；下方是三层铜烛架，共 24 支红色玻璃祈祷蜡烛。
  - 还没点蜡烛时，顶部面板显示提示「默祷之后，按一下点亮蜡烛」。
  - **点烛**：一点柔光从下方缓缓升起，飞到烛架上点亮一支蜡烛（火苗慢慢长出，周围泛起一圈光）；
    右上角「阿们」在原地柔和浮现再淡出，顶部面板显示「已点亮 N 支」。
  - 前 24 支依次点亮整个烛架，之后每次会重新点亮其中一支。
  - 连按时每一下都会计数，最多同时 6 点光在空中。
- 蜡烛数停止操作 1 秒后写入 Flash（NVS），断电重启后接着计数。数据只存在设备本地，不联网。

整体动画刻意保持安静：烛光、柔光、飘浮的光点，没有冲击线、印章、计分跳字之类的效果。

## 设计上的考虑

- **按键 ≠ 祷告**：把祷告做成「按一下算一次」容易显得机械、游戏化（参见马太福音 6:7「不可用许多重复话」），
  所以按键的含义是「默祷之后点亮一支蜡烛」，计数也是点亮的蜡烛数。
- **经文不作奖励**：经文在进入场景时作为「今日经文」出现，不设「满 N 次解锁」。
- **经文摘句**：取自和合本（神版，已进入公有领域）。摘句未到句末的以「……」结尾，不静默截断；
  请对照和合本核对字句后再对外发布。天主教用户习惯思高本和「天主」的称呼，如需要可替换 `tools/make_text.py` 里的经文。
- **图片**：开机图为 AI 生成的艺术形象，见下节的处理说明。

## 关于开机图

原图（`art/logo_orig.png`）背景右上角有一个苦像十字架。画面里是耶稣本人在祷告，身后再挂他被钉十字架的像，
时间和逻辑上不太协调，所以用 [`tools/fix_logo.py`](tools/fix_logo.py) 把那一块换成同一扇彩窗的纹理
（镜像、模糊、羽化融合），得到 `art/logo_src.png`，固件用的是处理后的图。

## 硬件与接线

与 [esp32-c3-muyu](https://github.com/tomdiynew/esp32-c3-muyu) 相同：ESP32-C3 SuperMini（BOOT 键 = GPIO9）+
FPC-1502401（GC9306）或 FPC-1502403（GC9307），开机自动识别。引脚在 [`main/board.h`](main/board.h)。

| 屏 FPC 引脚 | 信号 | ESP32-C3 |
|---|---|---|
| 1 | LCD_RES | GPIO4 |
| 2, 21 | GND | GND |
| 4 | LCD_CS | GPIO10 |
| 5 | LCD_SCL | GPIO6 |
| 7 | LCD_SDA | GPIO7 |
| 24 | LCD_DC | GPIO5 |
| 8, 15, 16 | LCD_3.3V | 3V3 |
| 11, 12 | LED_A | 3V3 经 10Ω |
| 13, 14 | LED_K | GND（或 N-MOSFET，栅极接 GPIO3） |

## 编译和烧录

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
git clone https://github.com/tomdiynew/esp32-c3-pray_c3.git
cd esp32-c3-pray_c3
idf.py set-target esp32c3        # 首次
idf.py build
idf.py -p COMx flash monitor
```

生成好的图片和字库都已入库，直接编译即可。

- 屏幕上的「开始祷告」和点烛都是按板载 **BOOT 键**（本项目没有接触摸）。
- 烧录不进时：按住 BOOT，点一下 RESET，松开 BOOT，再烧录。
- menuconfig 里 `Prayer → LCD panel` 可手动指定屏型号 / 反色，默认自动识别。
- 重新烧录不会清掉蜡烛数（保存在 NVS 分区）；想从 0 开始，长按 BOOT 1 秒，或执行 `idf.py -p COMx erase-flash` 后再烧录。

## 资源占用

| | 大小 |
|---|---|
| 固件 | 约 0.87 MB（程序分区 3.9 MB，剩 78%） |
| 开机图 + 祷告场景背景 | 2 × 134 KB |
| 精灵图（经文卡片、光束、点亮的烛杯、火苗、光点、光圈、「阿们」、按钮等） | 约 340 KB |
| 字库（华文楷体 17 px 经文 208 字、雅黑 13 px 出处与界面文字 151 字） | 约 31 KB |
| RAM | 静态约 60 KB + 送屏缓冲 2 × 19 KB |

## 美术与文字

- [`tools/fix_logo.py`](tools/fix_logo.py)：去掉原图背景里的苦像十字架（只需运行一次）。
- [`tools/make_art.py`](tools/make_art.py)：开机图缩放；祷告场景背景分三层合成——教堂（彩窗、光束、烛光虚化），
  从开机图裁出的祷告像贴进拱框，前景（金色拱框、烛架与未点亮的红色烛杯、计数面板）；其余精灵用 SVG 画，
  无头 Edge/Chrome 渲染成带 alpha 的精灵图。24 支蜡烛的点亮顺序也在这里定义。
- [`tools/make_text.py`](tools/make_text.py)：经文（和合本，已进入公有领域）与出处、界面文字，生成 `main/verses.h`
  （文字 + 只含用到的字的字库），同时输出 `art/verses.txt` 方便校对。
- 计数数字的字体（`main/font_ui_data.h`）由 `tools/make_ui_font.py` 生成，与电子木鱼相同。

修改后重新生成（需要 Windows + Python、Pillow，以及 Edge 或 Chrome）：

```powershell
python tools\make_art.py --preview art\preview.png
python tools\make_text.py
idf.py build
```

## 工程结构

```
main/
  main.c        开机 / 祷告两个画面、今日经文卡片、光点点烛与计数、NVS 保存 / 清零、中文排版
  lcd.c         SPI + GC9306/9307 初始化、自动识别
  button.c      BOOT 键事件
  ui.c          抗锯齿数字
  board.h       引脚
  sprites.h     生成的精灵表、蜡烛位置、布局常量
  verses.h      生成的经文与字库
  art/          生成的图片资源
components/esp_lcd_gc9306/   屏驱动组件
tools/          资源生成脚本
art/            原图 / 处理后的开机图、预览图、经文清单
```
