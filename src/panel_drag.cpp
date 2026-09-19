// panel_drag.cpp —— 见 panel_drag.h 的口径说明。
//
// 这里只有三件事：钳制规则、锚点算术、config.json 的读写。三件都不依赖渲染层，
// 所以可以被离线探针完整量一遍（tools/dragprobe.cpp）。

#include "panel_drag.h"

#include "json_min.h"
#include "renderer.h"   // kCanvasWidthDip / kCanvasHeightDip：画布尺寸的唯一出处

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace dshb {

namespace {

// 两个矩形的交集。空交集时返回一个"零面积"的矩形（right <= left），调用方按面积算即可。
RECT Intersect(const RECT& a, const RECT& b) {
    RECT r{};
    r.left = (a.left > b.left) ? a.left : b.left;
    r.top = (a.top > b.top) ? a.top : b.top;
    r.right = (a.right < b.right) ? a.right : b.right;
    r.bottom = (a.bottom < b.bottom) ? a.bottom : b.bottom;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

long long Area(const RECT& r) {
    const long long w = static_cast<long long>(r.right) - r.left;
    const long long h = static_cast<long long>(r.bottom) - r.top;
    if (w <= 0 || h <= 0) return 0;
    return w * h;
}

// 越界像素数（一个轴上最多只有一侧越界；面板比工作区还大时两侧都算）。
int Overshoot(int low, int high, int limitLow, int limitHigh) {
    int off = 0;
    if (low < limitLow) off += limitLow - low;
    if (high > limitHigh) off += high - limitHigh;
    return off;
}

bool ReadAllBytes(const std::wstring& path, std::string* out, std::wstring* why) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        if (why) *why = L"打不开文件（不存在或没有读权限）";
        return false;
    }
    char buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, got);
    std::fclose(f);
    return true;
}

// JSON 字符串必须重新转义：json_min 读的时候把 \uXXXX 解码成了 UTF-8 字节，
// 原样写回去就不是合法 JSON 了（引号与控制字符同样）。
void WriteJsonString(const std::string& text, std::string* out) {
    out->push_back('"');
    for (unsigned char c : text) {
        switch (c) {
        case '"': out->append("\\\""); break;
        case '\\': out->append("\\\\"); break;
        case '\b': out->append("\\b"); break;
        case '\f': out->append("\\f"); break;
        case '\n': out->append("\\n"); break;
        case '\r': out->append("\\r"); break;
        case '\t': out->append("\\t"); break;
        default:
            if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out->append(esc);
            } else {
                out->push_back(static_cast<char>(c));   // >= 0x80 的字节原样（本就是 UTF-8）
            }
            break;
        }
    }
    out->push_back('"');
}

// 把读出来的值写回 JSON。数字用**源文本**（json_min 从不让数字过浮点），
// 所以 240 写回去还是 240，不会被写成 2.4e+02。
void WriteJsonValue(const json::Value& v, std::string* out) {
    switch (v.kind) {
    case json::Kind::Null: out->append("null"); break;
    case json::Kind::Bool: out->append(v.boolean ? "true" : "false"); break;
    case json::Kind::Number: out->append(v.text.empty() ? "0" : v.text); break;
    case json::Kind::String: WriteJsonString(v.text, out); break;
    case json::Kind::Array: {
        out->push_back('[');
        for (size_t i = 0; i < v.items.size(); ++i) {
            if (i) out->push_back(',');
            WriteJsonValue(v.items[i], out);
        }
        out->push_back(']');
        break;
    }
    case json::Kind::Object: {
        out->push_back('{');
        for (size_t i = 0; i < v.members.size(); ++i) {
            if (i) out->push_back(',');
            WriteJsonString(v.members[i].first, out);
            out->push_back(':');
            WriteJsonValue(v.members[i].second, out);
        }
        out->push_back('}');
        break;
    }
    }
}

// 严格读一个整数成员：必须是 Number，且整段文本都是整数（"240.0" 也算不认 ——
// 这个文件只由本程序写，写进去的永远是整数；认了浮点就要开始猜精度）。
bool ReadIntMember(const json::Value& obj, const char* key, int* out, bool* found) {
    *found = false;
    const json::Value* v = obj.Find(key);
    if (!v) return true;                      // 没有这个成员：不算错，交给调用方判
    if (!v->IsNumber()) return false;
    const char* s = v->text.c_str();
    char* end = nullptr;
    const long value = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    if (value < -1000000L || value > 1000000L) return false;   // 离谱的值当损坏处理
    *out = static_cast<int>(value);
    *found = true;
    return true;
}

}  // namespace

PanelGeometry PanelForWindow(SIZE windowSize) {
    if (windowSize.cx <= 0 || windowSize.cy <= 0) return PanelGeometry{};

    // 两个轴**各按自己的轴**缩放（sx、sy），而不是都按 cx：等比窗口下两者相等，但
    // 非等比输入（探针合成、或将来某个开关只改一个轴）各自按轴算才不会把面板算歪。
    // 舍入口径与渲染器一致（renderer.cpp: m = kMarginDip×scale + 0.5 截断、
    // 实体 = kEntityWidthDip×scale + 0.5 四舍五入），这样"推出来的面板"与"画出来的面板"
    // 逐像素重合 —— 吸附贴的是真的那条边。
    const int margin = static_cast<int>(static_cast<double>(kMarginDip) * windowSize.cx /
                                        kCanvasWidthDip);
    const int width = static_cast<int>(static_cast<double>(kEntityWidthDip) * windowSize.cx /
                                       kCanvasWidthDip + 0.5);
    const int height = static_cast<int>(static_cast<double>(kEntityHeightDip) * windowSize.cy /
                                        kCanvasHeightDip + 0.5);
    return PanelGeometry{margin, width, height};
}

RECT PanelRectForWindow(POINT windowTopLeft, const PanelGeometry& panel) {
    return RECT{windowTopLeft.x + panel.margin, windowTopLeft.y + panel.margin,
                windowTopLeft.x + panel.margin + panel.width,
                windowTopLeft.y + panel.margin + panel.height};
}

WorkArea WorkAreaForPoint(POINT point) {
    WorkArea out{};
    // DEFAULTTONEAREST：点落在所有显示器之外时也给最近的那块（理由见头文件）。
    HMONITOR mon = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (!mon) return out;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return out;
    out.rect = mi.rcWork;
    out.valid = true;
    return out;
}

double VisibleAreaFraction(const RECT& rect, const RECT& workArea) {
    const long long whole = Area(rect);
    if (whole <= 0) return 0.0;
    const double visible = static_cast<double>(Area(Intersect(rect, workArea)));
    return visible / static_cast<double>(whole);
}

POINT SnapTopLeftIn(POINT desiredTopLeft, const PanelGeometry& panel, const RECT& workArea) {
    if (panel.width <= 0 || panel.height <= 0) return desiredTopLeft;
    if (workArea.right <= workArea.left || workArea.bottom <= workArea.top) return desiredTopLeft;

    // 未吸附时的面板矩形 —— 判据全部落在这四条边上（不是光标的位置）。
    const RECT entity = PanelRectForWindow(desiredTopLeft, panel);
    POINT out = desiredTopLeft;

    // 两块轴各判各的：两边都进范围时取**更近**的那条（面板比工作区还宽时才会发生）。
    const int gapLeft = entity.left - workArea.left;
    const int gapRight = workArea.right - entity.right;
    if (gapLeft <= kSnapDistancePx || gapRight <= kSnapDistancePx) {
        // 面板左 = 工作区左 → 窗口 x = 工作区左 − margin。整条式子只有一次整数运算，
        // 没有中间舍入：吸附后的面板边必须**恰好**落在工作区的边上，1 像素的偏差就是缺陷。
        const int snappedLeft = workArea.left - panel.margin;
        const int snappedRight = workArea.right - panel.margin - panel.width;
        out.x = (gapLeft <= gapRight) ? snappedLeft : snappedRight;
    }
    const int gapTop = entity.top - workArea.top;
    const int gapBottom = workArea.bottom - entity.bottom;
    if (gapTop <= kSnapDistancePx || gapBottom <= kSnapDistancePx) {
        const int snappedTop = workArea.top - panel.margin;
        const int snappedBottom = workArea.bottom - panel.margin - panel.height;
        out.y = (gapTop <= gapBottom) ? snappedTop : snappedBottom;
    }

    // ★ 这里**没有**"吸附不许把面板从光标底下抽走"那种上限。想过，也算过，它和判据打架：
    //   右/下两条边要贴住屏边，光标就得离那条屏边至少 margin + 面板宽 = 395px，而面板一共
    //   才 315px 宽 —— 也就是说"光标还在面板里"与"面板右边缘贴住屏右边"在同一个轴上是
    //   互斥的（左/上两条边不受影响，因为它要的是另一侧）。所有者定的吸附目标是"面板贴住
    //   屏边"，所以这里只按几何算：面板的边贴上去，光标可能相对面板滑一点。位置是**位置的
    //   纯函数**这一点不受影响（下一帧照光标重算，拖走立刻脱开）。
    return out;
}

POINT SnapTopLeft(POINT desiredTopLeft, const PanelGeometry& panel, POINT cursor) {
    const WorkArea area = WorkAreaForPoint(cursor);
    if (!area.valid) return desiredTopLeft;   // 拿不到工作区就不吸，绝不猜一条边
    return SnapTopLeftIn(desiredTopLeft, panel, area.rect);
}

POINT ClampWindowTopLeftIn(POINT desiredTopLeft, const PanelGeometry& panel,
                           const RECT& workArea) {
    if (panel.width <= 0 || panel.height <= 0) return desiredTopLeft;
    const int workW = workArea.right - workArea.left;
    const int workH = workArea.bottom - workArea.top;
    if (workW <= 0 || workH <= 0) return desiredTopLeft;

    // 半个面板**向上取整**：315 的一半是 157.5，取 157 时 157/315 = 49.84%，
    // 面积判据差一点点就不及格。取 158 -> 50.16%。分母是面板，所以取整也要按面板取。
    const int halfW = (panel.width + 1) / 2;
    const int halfH = (panel.height + 1) / 2;
    const int margin = panel.margin;   // "面板坐标 → 窗口坐标"的平移量

    // 面板比工作区还大（真机上不会有，探针的合成输入有）：任何位置都留不住一半，居中
    // 留得最多 —— 那时两个极限区在中间重合，"取更近的极限区"给出来的正好是居中。
    // ★ 判据取的是**面板**与工作区比，不是窗口与工作区比：窗口比屏宽、面板却放得下时
    //   （探针的合成输入就是这样），按窗口判会去居中，把放得下的面板推出屏外。
    const bool tooWide = panel.width >= workW;
    const bool tooTall = panel.height >= workH;

    // 每个轴至少留半个面板在工作区里 —— 这一条就是设计 §10.1 说的"允许一半悬在屏外"。
    // ★ 边界值必须用 (面板尺寸 − 半面板) 来算，不能用半个面板直接比：
    //   "左边越界"时留在屏内的宽度是 panel.left + 面板宽 − work.left，要求它 >= 半面板；
    //   写成 panel.left + 半面板 >= work.left 就错了一整块（窗口口径时代探针实测：
    //   喂 (-238,400) 时窗口停在 -238，可见面积 49.89%，差一点点不及格）。
    //   → 窗口边界 = 面板边界 − margin，这一步平移就是"面板坐标 → 窗口坐标"。
    const int minPanelLeft = workArea.left - (panel.width - halfW);
    const int maxPanelLeft = workArea.right - halfW;
    const int minPanelTop = workArea.top - (panel.height - halfH);
    const int maxPanelTop = workArea.bottom - halfH;

    int panelLeft = desiredTopLeft.x + margin;
    int panelTop = desiredTopLeft.y + margin;
    if (tooWide) {
        panelLeft = workArea.left + (workW - panel.width) / 2;
    } else {
        if (panelLeft < minPanelLeft) panelLeft = minPanelLeft;
        if (panelLeft > maxPanelLeft) panelLeft = maxPanelLeft;
    }
    if (tooTall) {
        panelTop = workArea.top + (workH - panel.height) / 2;
    } else {
        if (panelTop < minPanelTop) panelTop = minPanelTop;
        if (panelTop > maxPanelTop) panelTop = maxPanelTop;
    }

    RECT entity{panelLeft, panelTop, panelLeft + panel.width, panelTop + panel.height};
    if (VisibleAreaFraction(entity, workArea) + 1e-9 >= kMinVisibleAreaFraction) {
        return POINT{entity.left - margin, entity.top - margin};
    }

    // ★ 逐轴的"各留一半"**不**蕴含面积 >= 50%：两个轴同时压到各自的半面板极限时面积只有
    //   25%（0.5 × 0.5）。判据要的是**面积**，所以这里必须再补一次：把"越界更少的那个轴"
    //   整个收进工作区 —— 收完那个轴是满的、另一个轴是半的，面积恰好 50%。
    //   选越界少的那个轴，是为了让面板尽量少动（用户看到的是"它停在边上"，不是"被弹回来"）。
    //   这条不是理论分支：四个角就是这样（面板半个宽、半个高同时悬在屏外），
    //   探针 clamp-area-invariant 会把它量出来。
    const int offW = Overshoot(entity.left, entity.right, workArea.left, workArea.right);
    const int offH = Overshoot(entity.top, entity.bottom, workArea.top, workArea.bottom);
    if (offW <= offH) {
        entity.left = (entity.left < workArea.left) ? workArea.left : workArea.right - panel.width;
        entity.right = entity.left + panel.width;
    } else {
        entity.top = (entity.top < workArea.top) ? workArea.top : workArea.bottom - panel.height;
        entity.bottom = entity.top + panel.height;
    }
    return POINT{entity.left - margin, entity.top - margin};
}

POINT ClampWindowTopLeft(POINT desiredTopLeft, const PanelGeometry& panel, POINT cursor) {
    const WorkArea area = WorkAreaForPoint(cursor);
    if (!area.valid) return desiredTopLeft;   // 拿不到工作区就不动它，绝不猜一个边界
    return ClampWindowTopLeftIn(desiredTopLeft, panel, area.rect);
}

POINT DragOffsetFor(const RECT& window, POINT cursor) {
    // 口径：光标 − 窗口左上角。反过来（窗口 − 光标）在屏中间看起来一样，一直到窗口被
    // 钳制在边上、光标跑到窗口外面那一刻 —— 那时符号错了，窗口会朝相反方向跑。
    return POINT{cursor.x - window.left, cursor.y - window.top};
}

POINT DragTopLeft(POINT dragOffset, POINT cursor) {
    // 位置是光标坐标的**纯函数**：不累加、不缓动、不看上一帧。所以"跟手不漂"是恒等式，
    // 不是"调得好"。
    return POINT{cursor.x - dragOffset.x, cursor.y - dragOffset.y};
}

bool LoadWindowPos(const std::wstring& path, SIZE windowSize, POINT* outTopLeft,
                   std::wstring* why) {
    std::string text;
    if (!ReadAllBytes(path, &text, why)) return false;

    const json::Outcome parsed = json::Parse(text);
    if (!parsed.ok) {
        if (why) *why = L"JSON 解析失败：" + std::wstring(parsed.error.begin(), parsed.error.end());
        return false;
    }
    if (!parsed.root.IsObject()) {
        if (why) *why = L"顶层不是对象";
        return false;
    }
    int x = 0;
    int y = 0;
    bool hasX = false;
    bool hasY = false;
    if (!ReadIntMember(parsed.root, "windowX", &x, &hasX) ||
        !ReadIntMember(parsed.root, "windowY", &y, &hasY)) {
        if (why) *why = L"windowX/windowY 不是整数";
        return false;
    }
    if (!hasX || !hasY) {
        if (why) *why = L"缺 windowX 或 windowY";
        return false;
    }

    // 这个位置还算不算数？判据与钳制**同一条**：最近的显示器工作区里至少一半的**面板**面积。
    // ★ 不能只判"左上角在某块显示器上"：钳制本身就允许面板一半悬在屏外，那种合法位置的
    //   左上角可能已经在屏外 —— 按左上角判会把它当坏数据丢掉，用户看到的是
    //   "拖到屏幕边上，重开又回到中间"。
    // ★ 这里量与吸附**同一个矩形**（面板，不是窗口外框）。差这一步的后果是具体的：
    //   面板贴住屏边时窗口外框已经越界 80px，若按外框算，那种位置的可见面积会掉到一半
    //   以下被判非法 —— 用户看到"贴着边放好、重开却回到屏幕中间"。
    const PanelGeometry panel = PanelForWindow(windowSize);
    const WorkArea area = WorkAreaForPoint(POINT{x, y});
    const RECT panelRect = PanelRectForWindow(POINT{x, y}, panel);
    if (!area.valid || VisibleAreaFraction(panelRect, area.rect) < kMinVisibleAreaFraction) {
        if (why) {
            wchar_t buf[128];
            swprintf_s(buf, L"(%d,%d) 处没有任何显示器能留下 >= %.0f%% 的**面板**面积", x, y,
                       kMinVisibleAreaFraction * 100.0);
            *why = buf;
        }
        return false;
    }
    *outTopLeft = POINT{x, y};
    return true;
}

bool SaveWindowPos(const std::wstring& path, POINT topLeft, std::wstring* why) {
    // 1) 先把文件里**已经有的**成员读出来（不存在或损坏 -> 当空对象，后面整份重写）。
    //    为什么必须保留：这是用户的设置文件，以后会加字段；这个版本在改位置时把别人的
    //    字段吃掉，等于每次拖动都悄悄回退一次别人的设置。
    std::vector<std::pair<std::string, json::Value>> members;
    {
        std::string text;
        std::wstring ignored;
        if (ReadAllBytes(path, &text, &ignored)) {
            const json::Outcome parsed = json::Parse(text);
            if (parsed.ok && parsed.root.IsObject()) members = parsed.root.members;
        }
    }

    json::Value xValue;
    xValue.kind = json::Kind::Number;
    xValue.text = std::to_string(static_cast<long long>(topLeft.x));
    json::Value yValue;
    yValue.kind = json::Kind::Number;
    yValue.text = std::to_string(static_cast<long long>(topLeft.y));

    bool wroteX = false;
    bool wroteY = false;
    for (auto& m : members) {
        if (m.first == "windowX") { m.second = xValue; wroteX = true; }
        if (m.first == "windowY") { m.second = yValue; wroteY = true; }
    }
    if (!wroteX) members.emplace_back("windowX", xValue);
    if (!wroteY) members.emplace_back("windowY", yValue);

    std::string text = "{\n";
    for (size_t i = 0; i < members.size(); ++i) {
        text += "  ";
        WriteJsonString(members[i].first, &text);
        text += ": ";
        WriteJsonValue(members[i].second, &text);
        if (i + 1 < members.size()) text += ",";
        text += "\n";
    }
    text += "}\n";

    // 2) 先写临时文件、再原子替换。为什么值得多这一步：这个文件是**跨进程**的边界
    //    （下一次启动要读它）。写一半被杀掉会在盘上留半个 JSON，下次启动按"文件损坏"
    //    退回 240,240 —— 用户看到的现象是"位置偶尔会丢"，而原因是自己踩到的。
    const std::wstring tempPath = path + L".tmp";
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, tempPath.c_str(), L"wb") != 0 || !f) {
            if (why) *why = L"建不了临时文件（目录不存在或不可写）";
            return false;
        }
        const size_t written = std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        if (written != text.size()) {
            DeleteFileW(tempPath.c_str());
            if (why) *why = L"写临时文件不完整";
            return false;
        }
    }
    if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = GetLastError();
        DeleteFileW(tempPath.c_str());
        if (why) {
            wchar_t buf[96];
            swprintf_s(buf, L"替换目标文件失败 err=%lu", err);
            *why = buf;
        }
        return false;
    }
    return true;
}

}  // namespace dshb
