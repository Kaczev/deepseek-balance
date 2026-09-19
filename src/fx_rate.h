// fx_rate.h -- 一个会话只取一次的 USD->CNY 汇率，以及"给只有 CNY 的样本补一个 USD 条目"。
//
// 所有者要的东西（逐字）："我们的软件其实是支持 USD 显示的。但是现在 deepseek 的大陆账号
// 返回的是 CNY。现在我们的软件按一下那个价格符号就可以切换到美元显示。"
// 他自己定的两条：**只在启动时取一次汇率**；曲线那边"应该是加一个 USD 就可以正常使用"。
// （"只在启动时取一次"说的是**发起请求的次数**：整条进程生命周期里最多一次 HTTP 请求，
//  不存在周期轮询；启动时若已有一份缓存，也仍然是"最多请求一次"。）
//
// ★ 设计要点：**不在显示层另写一套换算**。
//   汇率只在"样本从后台线程出来"的那一刻用一次，产出的是一个**真币种条目**
//   （CurrencyAmount{currency="USD", total=..., ok=true}），和接口自己给的 USD 条目
//   形状完全一样。于是下游 —— 显示、切换、曲线、预估、落盘 —— 一行都不用改:
//     * DisplayedAmount::OnSample 只是多了一条能选的币种（它的 lastEntries_ 直接吃）
//     * FeedCurve 把 entries 逐条写成 CurveObservation::Item，落盘就是 {"CNY":..,"USD":..}
//     * useToZero 预估按选中币种取点（CurvePointSeries），也自然能取到 USD
//   如果换算写在显示层，曲线里就永远只有 CNY —— 所有者说的"加一个 USD 就能用"正是
//   "它必须是一个真条目"这句话的另一种说法。
//
// ★ 为什么取汇用 frankfurter.app（ECB 的参考汇率）而不是 er-api 那个聚合站：
//   1. 它**不需要 API key**。要 key 的免费接口等于把"我能不能取到汇率"绑在另一个
//      账号的配额上，而这里要的是一个"随便谁开机都能成"的默认值。
//   2. 它是 ECB 官方参考汇率（frankfurter 只做 ECB 每日一次的转发，不做自己的牌价）。
//      对"一个会话里固定不变的一个数"来说，官方中间价比第三方聚合价更可解释。
//   3. 它的粒度就是"每天一次"，和所有者定的"启动时取一次"同频：不会出现同一个会话里
//      两次请求给出两个不同答案的问题（那个聚合站是每日一次，但对外宣称接近实时）。
//   代价是它更新不如聚合站快（ECB 每工作日约 16:00 CET）。这个代价换的是可解释性，
//   而且汇率在这里只用来把 CNY 显示成 USD —— 它不是账目。
//
// ★ 失败就是失败：取不到 = **本次会话没有汇率**，样本里不补 USD，显示退回
//   "--.--" 加 "$"（切换机制本来就是这个行为），不做重试、不做回落值、不猜。
//
// ★★ 2026-09-19 追加（所有者当场定的，优先级高于上面那句兜底）："取不到时可以用上一次的
//    吗？反正汇率变化不是很大"。他自己的情况是"不一定一直开着这个软件"，所以开机那一刻
//    取不到网络时，**上一次存下来的那个值往往是唯一能用的那个**。于是回退顺序是：
//        本次取汇 -> 磁盘缓存 -> （都没有）没有汇率
//    缓存落在数据目录（paths.h 那一层，和 config.json / curve.json 同一个目录），
//    内容 = 汇率值 + 牌价日期 + 取到的时间。写法照 panel_drag.cpp 的 SaveWindowPos：
//    临时文件 + MoveFileEx 原子替换 —— 这是**跨进程**的边界（下一次启动要读它），
//    写一半被杀掉会留下半个 JSON。
//    缓存有上限（kCacheMaxAgeDays）：超过就当作没有，并在日志里写明为什么丢弃。
//    理由：汇率不是每天大变，但一个 30 天前的值拿去换算会**静默地骗人**，而"没有汇率"
//    至少是诚实的 —— 屏幕上会出现 --.--，人一眼就知道这个数没有来源。
#pragma once

#include "sampling.h"

#include <string>

namespace dshb::fx {

// 缓存的新鲜度上限（天）。所有者定的是 30 天；判断：ECB 参考汇率按月波动通常在个位数
// 百分比，30 天换算一个 20 元的余额，误差仍在"分"的量级 —— 也就是说它不会把一个
// 数字变成另一个数字，只会让末位一分钱有偏差。再长就不成立了，所以取 30 而不是无限。
inline constexpr int kCacheMaxAgeDays = 30;

// 汇率接口的地址。写成结构体而不是常量，理由和 api_client::Endpoint 一样：
// 失败路径必须在**不重编译**的前提下可复发（指向一个故意连不上的本地端口即可），
// 否则"取不到汇率不崩、不卡"这句话就只能靠读代码相信。
struct RateEndpoint {
    // ★ 地址实测（2026-09-19）：老域名 api.frankfurter.app 现在对 /latest 回 **301**，
    //   而本项目的传输层刻意**不跟随重定向**（http_get.h 里写了为什么：一次请求必须
    //   恰好打到它被指定的地方）。301 会被当成"取不到汇率"，于是走缓存回退。
    //   现行地址是 api.frankfurter.dev/v1/latest，参数名也从 from/to 变成了
    //   base/symbols —— 响应形状没变：
    //     {"amount":1.0,"base":"USD","date":"2026-09-18","rates":{"CNY":6.6976}}
    std::wstring host = L"api.frankfurter.dev";
    unsigned short port = 443;
    bool secure = true;                 // false 只有本地明文测试服务器用
    std::wstring path = L"/v1/latest?base=USD&symbols=CNY";
    // 5 s，和余额请求同一个量级。★ 这个超时是**硬要求**：取汇在这一条线程上、
    // 在第一次采样之前，所以它最长就是把第一次采样推迟这么久。
    int timeoutMs = 5000;
};

struct Rate {
    bool ok = false;            // false = 本会话没有可用汇率（唯一的失败表示）
    std::string rateText;       // 逐字来自响应的汇率文本，例如 "6.6976"（不重排格式）
    std::string date;           // 响应里的牌价日期，例如 "2026-09-18"；可能为空
    std::string error;          // ok=false 时的原因，短、只进日志

    // ---- 缓存（2026-09-19 追加）----
    bool cached = false;        // true = 本次取汇失败，用的是磁盘上那一个
    std::string fetchedAt;      // 缓存被取到的时刻 "YYYY-MM-DD HH:MM"（本地时间）
    long long fetchedAtUnix = 0;// 同上，机器可读；仅用于算年龄
    long long ageDays = -1;     // 缓存的年龄（天）；-1 = 不是来自缓存
    std::string fallbackNote;   // 走到缓存的原因（本次取汇为什么失败）
    // 本次取到了、但没能写进缓存时的原因。空 = 没出这件事。
    // ★ 单独一个字段而不是复用 fallbackNote：前者是"我没有新值"，后者是"我有新值但下一次
    //   开机不会有它"，两者的日志措辞和严重程度都不同，混在一起就会写出误导人的那一行。
    std::string cacheSaveError;

    // ---- 转换（纯函数，无网络）----
    // 用**定点整数**做除法，不走二进制浮点：金额与汇率都是它们自己的十进制文本
    // （设计 §3.1 的规矩，amount.h 顶部写了为什么），一次浮点舍入就足以让
    // "USD = CNY / rate" 这句断言在末位上对不上。
    //
    // 舍入方向：**截断**到分（两位小数），不做四舍五入。
    // 理由：换算出来的 USD 是给人看的一个显示量，不是账目；两种方向都说得过去，
    // 所以选"绝不把余额显示得比实际更大"的那一种。
    std::string ConvertAmountText(const std::string& cnyText) const;

    // 一行日志（启动过渡要能看出汇率来自哪、值是多少）。
    // 例：[fx] 1 USD = 6.6976 CNY（frankfurter/ECB 2026-09-18）
    std::string LogLine() const;
};

// 解析汇率响应的正文。纯函数：同样的字节永远给同样的结果。
//   { "amount":1.0, "base":"USD", "date":"2026-09-18", "rates":{"CNY":6.6976} }
// 不接受：非 JSON、base 不是 USD、rates 缺失/不是对象、CNY 缺失/不是数/<= 0。
// 一律 ok=false 并把原因写进 error —— **绝不**回落到一个猜出来的汇率。
Rate ParseRateBody(const std::string& body);

// 取一次汇率。**同步、会阻塞调用线程**：它必须从 BalanceSource 的后台线程里调，
// 绝不能在 UI 线程上调（一次 400 ms 的请求就是几十帧）。
Rate FetchRate(const RateEndpoint& endpoint);
Rate FetchRate();   // 默认地址（frankfurter/ECB）

// ---------------------------------------------------------------------------
// 缓存：一个会话只写一次、每次启动读一次的小文件
// ---------------------------------------------------------------------------

// 读缓存。path 为空 -> 直接 ok=false（"没有缓存"）。
// 什么时候算没有缓存：文件不存在 / 不是 JSON / 形状不对（三个成员缺一个）/ 汇率不是正数 /
// **年龄超过 kCacheMaxAgeDays** / 取到的时间读不出来（年龄无从判断 —— 一个看不出多旧的
// 值和一个 30 天旧的值一样不能信）。每种情况都写进 error，日志照抄。
Rate LoadCachedRate(const std::wstring& path, long long nowUnixSeconds);

// 写缓存（临时文件 + MoveFileEx 原子替换，照 panel_drag.cpp 的 SaveWindowPos）。
// 返回是否写成，why 里是失败原因（只进日志）。
bool SaveCachedRate(const std::wstring& path, const Rate& rate, long long nowUnixSeconds,
                    std::wstring* why);

// ---------------------------------------------------------------------------
// 一次"取汇 + 回退"的完整决定：**后台线程只调这一个**
// ---------------------------------------------------------------------------
// * 取到了          -> ok=true，并且把它写进缓存（cached=false）
// * 取不到、有缓存  -> ok=true，cached=true，fallbackNote = 失败原因（日志要能看出用了哪个）
// * 取不到、没缓存  -> ok=false，error 里同时有"本次为什么失败"和"没有缓存"
// ★ 放在这里而不是调用方：这条规则是汇率自己的规则（顺序、上限、日志措辞），
//   调用方只该拿到"一个可用的汇率 + 它是怎么来的"。
Rate ResolveRate(const RateEndpoint& endpoint, const std::wstring& cachePath,
                 long long nowUnixSeconds);
Rate ResolveRate(const RateEndpoint& endpoint, const std::wstring& cachePath);

// 当前 Unix 秒（本地时钟）。单独一个函数是为了让探针能给 ResolveRate/LoadCachedRate
// 喂一个假"现在"，不然"缓存过期"这条只能靠改系统时钟来验。
long long NowUnixSeconds();

// 给样本补一个 USD 条目。返回是否补了。
//   * entries 里**已经有 USD**（海外账号）-> 一个字节都不动，返回 false
//   * 没有 USD、但有可用的 CNY、且 rate.ok       -> 追加 USD = CNY / rate，两位小数，ok=true
//   * 没有汇率、或没有可用的 CNY                 -> 不动，返回 false
// USD 追加在**末尾**，不是插在最前：primary 条目（曲线记点用哪个币种判定"变没变"、
// 前向预估读哪个币种）因此仍然是接口自己给的那一条，海外账号与大陆账号在这个位置上的
// 行为都不会因为本功能而改变。
bool InjectUsdCounterpart(Sample* sample, const Rate& rate);

}  // namespace dshb::fx
