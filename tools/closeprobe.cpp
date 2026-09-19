// closeprobe -- 关闭态（设计 §10.2）的状态机、R 的下限与"紧张"抖动的离线证明。
//
//   closeprobe            跑全部检查
//   closeprobe --verbose  另打印每一档抖动的逐帧幅度
//
// 一句话：**右键进入 → 三次点击即关 → 只有取消能把进度归零**，而第 1/2 击各把 R 的
// 下限抬到 0.5 / 1.0（光晕颜色与心跳周期、幅度都读同一个 R）。
//
// ★ 为什么这条性质要离线证：真机上 R 只抬不降（R(t)=a^t，抬升靠数据刷新或这两次点击），
//   而进程冷启动时 R = 1.0（ambienceSeconds_ 初值 0）。要看到"第 1 击把 R 抬到 0.50"，
//   真机上必须先等 R 衰减到 0.5 以下 —— 那是 75 秒的墙钟。这里用同一份 DisplayedAmount
//   把 100 秒的帧一次跑完（每帧 Update(1/60)），于是同一条判据在毫秒内可复现。
//   真机那一次（--shutdown-test=1 --shutdown-enter-at=75）是**端到端**的对照，两条都要有。
// ★ 它不画像素：这里量的是"交给渲染层的数"（R、颜色、心跳幅度/周期、抖动位移），
//   每一个像素都是这些数的函数。像素证据在导帧那一侧（见验收报告）。
#include "widget_display.h"

#include "amount.h"
#include "curve_store.h"
#include "heartbeat.h"
#include "sampling.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Harness {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    bool Req(const char* id, const std::string& what, const std::string& evidence, bool ok) {
        std::string line = std::string(ok ? "PASS: " : "FAIL: ") + id + " | " + what;
        if (!evidence.empty()) line += " | " + evidence;
        std::printf("%s\n", line.c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(line);
        }
        return ok;
    }
};

std::string F4(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.4f", v);
    return b;
}

std::string F10(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.10f", v);
    return b;
}

std::string HexOf(const dshb::AmbienceColor& c) {
    char b[16];
    std::snprintf(b, sizeof(b), "#%02x%02x%02x", static_cast<int>(c.r * 255.0f + 0.5f),
                  static_cast<int>(c.g * 255.0f + 0.5f), static_cast<int>(c.b * 255.0f + 0.5f));
    return b;
}

// 一条一直上涨的余额序列（所有者给的第 1/2 击验收场景：R_new 很低）。
// 每秒一步、每步 +0.01 元：StepPerMinute > 0 -> SeverityRatio = 0 -> R_new = 0。
dshb::Sample RisingSample(int seconds) {
    dshb::Sample s;
    s.wallMs = 1700000000000LL + static_cast<int64_t>(seconds) * 1000LL;
    s.monotonicMs = static_cast<int64_t>(seconds) * 1000LL;
    s.total = dshb::Amount::FromYuanDouble(100.0 + 0.01 * seconds);
    s.granted = dshb::Amount::FromYuanDouble(0.0);
    s.toppedUp = dshb::Amount::FromYuanDouble(0.0);
    s.amountsOk = true;
    s.currency = "CNY";
    s.isAvailable = true;
    return s;
}

// 跑 seconds 秒的帧（60 Hz），每秒喂一个上涨样本。返回最后一帧的 R（原值，未经状态规则）。
void RunSeconds(dshb::DisplayedAmount* d, int fromSeconds, int seconds) {
    for (int sec = 0; sec < seconds; ++sec) {
        d->OnSample(RisingSample(fromSeconds + sec));
        for (int f = 0; f < 60; ++f) d->Update(1.0 / 60.0);
    }
}

// 一条**陡降**样本（余额掉 2 元，间隔 1 s = -120 元/分，远陡于基线 -> R_new = 1）。
// ★ 为什么需要它（2026-09-19）：R 的计时器**只在第一次真的量出一步时才开始走**
//   （见 widget_display.h 的 ambienceSeconds_ 说明），所以一个刚起、还没量到任何一步的
//   进程是 R = 0，而不是 a^0 = 1。那正是所有者报的"每次点开都是红的"的修复。
//   于是"R 已经衰减到 0.5 以下"这个**前置状态**在探针里必须自己造出来：
//   先喂一次陡降把 R 抬到 1，再让它按 a^t 自己凉下去 —— 与真机上发生的事逐字相同。
//   不带这一步，下面的 case1/case6 量到的是 R = 0（"没抬起来过"，当然不会退）。
void SeedAndDecay(dshb::DisplayedAmount* d, int decaySeconds) {
    dshb::Sample drop;
    drop.wallMs = 1700000000000LL;
    drop.monotonicMs = 0;
    drop.total = dshb::Amount::FromYuanDouble(100.0);
    drop.amountsOk = true;
    drop.currency = "CNY";
    drop.isAvailable = true;
    d->OnSample(drop);
    dshb::Sample after;
    after.wallMs = 1700000001000LL;
    after.monotonicMs = 1000;
    after.total = dshb::Amount::FromYuanDouble(98.0);
    after.amountsOk = true;
    after.currency = "CNY";
    after.isAvailable = true;
    d->OnSample(after);
    for (int f = 0; f < 60; ++f) d->Update(1.0 / 60.0);
    RunSeconds(d, 100, decaySeconds);
}

double PeakToPeak(const std::vector<double>& v) {
    double lo = v[0], hi = v[0];
    for (double x : v) { if (x < lo) lo = x; if (x > hi) hi = x; }
    return hi - lo;
}

}  // namespace

int main(int argc, char** argv) {
    const bool verbose = (argc > 1 && std::string(argv[1]) == "--verbose");
    dshb::SetCurveStorePath(L"C:\\Users\\Kaczev\\AppData\\Local\\Temp\\dshb-close\\closeprobe-curve.json");  // 夹具不碰所有者的 curve.json

    Harness h;
    std::printf("closeprobe: 关闭态的状态机、R 的下限、抖动的纯函数性\n");
    std::printf("tuning: kShutdownRd1=%.2f kShutdownRd2=%.2f kShutdownEntryFrames=%d "
                "jitter(档1/2/3)=%.1f/%.1f/%.1f DIP\n",
                dshb::kShutdownRd1, dshb::kShutdownRd2, dshb::kShutdownEntryFrames,
                dshb::kShutdownJitterDip[0], dshb::kShutdownJitterDip[1],
                dshb::kShutdownJitterDip[2]);
    std::printf("scope: 量 src/widget_display.cpp 的关闭态（自由函数 + DisplayedAmount）；"
                "宿主把左/右键都翻译成同一个 ShutdownClick、把托盘菜单「关闭」翻译成"
                " ShutdownFireNow（那两条由真机剧本证）\n");

    // ---- 1) 起点：平静运行的挂件（一次陡降已经过去 100 s，余额一直上涨 -> R_new = 0）----
    dshb::DisplayedAmount d;
    SeedAndDecay(&d, 100);
    const double rBefore = d.ambienceRatioShown();
    const std::string colorBefore = HexOf(d.ambienceColor());
    h.Req("case0", "进入关闭态之前：R 已经衰减到 0.5 以下，且 R_d = 0（下限还没抬起来）",
          "一次陡降之后跑了 100 s 的上涨样本：R=" + F10(rBefore) + " color=" + colorBefore +
              " R_d=" + F4(dshb::ShutdownFloorRatio()) + " active=" +
              std::string(dshb::ShutdownActive() ? "yes" : "no"),
          rBefore < 0.5 && dshb::ShutdownFloorRatio() == 0.0 && !dshb::ShutdownActive());

    // ---- 1b) 冷启动：一步都还没量到时 R = 0（不是 a^0 = 1）—— 所有者报的"每次点开都是红的"
    //      就是这一条。判据分两半，两半都得成立：
    //        (a) 一个样本都没喂：R = 0、且数字读不到（--.-- 那一态本来就规定 R = 0）；
    //        (b) **喂了平坦余额之后仍然 R = 0** —— 这一半才是修复的实质：修复前
    //            第一个样本一进来，`hasValue_` 变真、"读不到 -> R = 0"那条规则让位，
    //            屏幕直接血红（实测 R 一步跳到 0.9749，见 .dsh/scratch/bug2_probe.cpp）。
    {
        dshb::DisplayedAmount cold;
        for (int f = 0; f < 12; ++f) cold.Update(1.0 / 60.0);
        const double rCold = cold.ambienceRatioShown();
        const bool unreadableCold = cold.ambienceUnreadable();
        // 平坦余额：与"起点"同一个值 -> 没有一步可量 -> R 必须还是 0。
        dshb::Sample flat;
        flat.wallMs = 1700000000000LL;
        flat.monotonicMs = 0;
        flat.total = dshb::Amount::FromYuanDouble(50.0);
        flat.amountsOk = true;
        flat.currency = "CNY";
        flat.isAvailable = true;
        cold.OnSample(flat);
        for (int f = 0; f < 12; ++f) cold.Update(1.0 / 60.0);
        const double rFlat = cold.ambienceRatioShown();
        const std::string colorFlat = HexOf(cold.ambienceColor());
        h.Req("case1b", "冷进程：一个样本都没喂时 R = 0；喂了**平坦**余额之后 R 仍然是 0"
                        "（修复前这里会跳到 ~0.97，屏幕血红）",
              "冷启动 12 帧: R=" + F10(rCold) + " unreadable=" +
                  std::string(unreadableCold ? "yes" : "no") + "；喂平坦 50.00 之后: R=" +
                  F10(rFlat) + " 颜色=" + colorFlat,
              rCold == 0.0 && unreadableCold && rFlat == 0.0);
    }

    // ---- 2) 右键进入：画面换掉，但 R 一动不动（还没点）----
    const bool entered = dshb::ShutdownEnter();
    d.Update(1.0 / 60.0);
    h.Req("case1", "右键进入：状态机进 Armed、帧号从 0 开始，R_d 仍为 0（第 1 击才做那次运算），"
                   "R 只按时间走了一帧（1/60 s 的衰减）",
          "entered=" + std::string(entered ? "yes" : "no") + " frame=" + std::to_string(dshb::ShutdownFrame()) +
              " clicks=" + std::to_string(dshb::ShutdownClicks()) + " R_d=" + F4(dshb::ShutdownFloorRatio()) +
              " R=" + F10(d.ambienceRatioShown()) + "（进入前 " + F10(rBefore) + "）",
          entered && dshb::ShutdownActive() && !dshb::ShutdownFired() && dshb::ShutdownClicks() == 0 &&
              dshb::ShutdownFrame() == 0 && dshb::ShutdownFloorRatio() == 0.0 &&
              d.ambienceRatioShown() < rBefore && (rBefore - d.ambienceRatioShown()) < 1e-3);

    // ---- 3) 第 1 击：R = max(R_new, R_d1) = 0.5，颜色 #f6aa6c，心跳按 R=0.5 取律 ----
    const bool fired1 = dshb::ShutdownClick();
    d.Update(1.0 / 60.0);
    const double r1 = d.ambienceRatioShown();
    const std::string color1 = HexOf(d.ambienceColor());
    const double a1 = dshb::BeatAmplitudePx(r1, d.ambienceDepthShown());
    const double t1 = dshb::BeatPeriodSeconds(r1, d.ambienceDepthShown());
    h.Req("case2", "第 1 击：R_d=0.50 -> R 恰好 0.50（颜色 #f6aa6c），心跳 5.2932px / 5.6256s",
          "fired=" + std::string(fired1 ? "yes" : "no") + " R_d=" + F4(dshb::ShutdownFloorRatio()) +
              " R_new=" + F10(d.ambienceRatioTarget()) + " R=" + F10(r1) + " color=" + color1 +
              " A=" + F4(a1) + "px T=" + F4(t1) + "s depth=" + F4(d.ambienceDepthShown()),
          !fired1 && std::fabs(r1 - dshb::kShutdownRd1) < 1e-9 && color1 == "#f6aa6c" &&
              std::fabs(a1 - 5.2932) < 0.001 && std::fabs(t1 - 5.6256) < 0.001);

    // ---- 4) 第 2 击：R = max(R_new, R_d2) = 1.0，颜色 #f66c6c，心跳 6.0px / 0.5s ----
    const bool fired2 = dshb::ShutdownClick();
    d.Update(1.0 / 60.0);
    const double r2 = d.ambienceRatioShown();
    const std::string color2 = HexOf(d.ambienceColor());
    const double a2 = dshb::BeatAmplitudePx(r2, d.ambienceDepthShown());
    const double t2 = dshb::BeatPeriodSeconds(r2, d.ambienceDepthShown());
    h.Req("case3", "第 2 击：R_d=1.00 -> R 恰好 1.00（颜色 #f66c6c），心跳 6.0000px / 0.5000s",
          "fired=" + std::string(fired2 ? "yes" : "no") + " R_d=" + F4(dshb::ShutdownFloorRatio()) +
              " R=" + F10(r2) + " color=" + color2 + " A=" + F4(a2) + "px T=" + F4(t2) + "s",
          !fired2 && std::fabs(r2 - dshb::kShutdownRd2) < 1e-9 && color2 == "#f66c6c" &&
              std::fabs(a2 - 6.0) < 1e-9 && std::fabs(t2 - 0.5) < 1e-9);

    // ---- 5) 没有时间窗：第 2 击之后跑 10 分钟，状态一位都不变、R 也不掉 ----
    // ★ 这里**不点第 3 击**：点了就把状态钉在 Fired，而 Fired 之后按设计不再受理任何输入
    //   （取消也不行），后面那两组（三档取消、进入段）就没有机会跑了。第 3 击那一组
    //   自己开场、放在最后。这条顺序是实测踩出来的：先点下去的版本里 case6/case8 全红。
    RunSeconds(&d, 200, 600);                 // 600 s：远超过任何"超时清零"的实现
    const double r2later = d.ambienceRatioShown();
    const bool stillArmed = dshb::ShutdownActive() && !dshb::ShutdownFired() && dshb::ShutdownClicks() == 2;
    h.Req("case4", "没有时间窗：第 2 击之后等 600 s，状态仍是 2 击未关，且**下限把 R 钉住**（不随时间掉）",
          "after 600s: active=" + std::string(dshb::ShutdownActive() ? "yes" : "no") + " clicks=" +
              std::to_string(dshb::ShutdownClicks()) + " R=" + F10(r2later) +
              "（600 s 里 R 一步都没掉 = 抬升规则每帧把它重新抬回 R_d）",
          stillArmed && std::fabs(r2later - dshb::kShutdownRd2) < 1e-9);
    dshb::ShutdownCancel();                   // 把状态放回 Off，交给下面两组

    // 第 3 击那一组放在**最后**：它把模块级状态钉在 Fired，而 Fired 之后按设计不再受理
    // 任何输入（取消也不行），所以取消/进入那两组必须在它之前跑。见文件末尾。

    // ---- 7) 取消：三档都能取消，进度归零、R_d 归零、R 交回时间 ----
    {
        std::string detail;
        bool ok = true;
        for (int tier = 0; tier <= 2; ++tier) {
            dshb::DisplayedAmount c;
            SeedAndDecay(&c, 100);
            dshb::ShutdownEnter();
            for (int k = 0; k < tier; ++k) dshb::ShutdownClick();
            const double rAtTier = c.ambienceRatioShown();
            const bool cancelled = dshb::ShutdownCancel();
            c.Update(1.0 / 60.0);
            const double rAfter = c.ambienceRatioShown();
            const bool reset = cancelled && !dshb::ShutdownActive() && dshb::ShutdownClicks() == 0 &&
                               dshb::ShutdownFrame() == -1 && dshb::ShutdownFloorRatio() == 0.0;
            // "R 交回时间"：取消之后再跑 60 s，R 必须往下走（下限没了，不再被钉住）
            RunSeconds(&c, 100, 60);
            const double rDecayed = c.ambienceRatioShown();
            const bool releases = rDecayed < rAtTier - 1e-6;
            ok = ok && reset && releases;
            detail += "档" + std::to_string(tier) + "(点" + std::to_string(tier) +
                      "下): 取消=" + std::string(cancelled ? "yes" : "no") + " 归零=" +
                      std::string(reset ? "yes" : "no") + " 取消前R=" + F4(rAtTier) +
                      " 取消后R=" + F4(rAfter) + " 再跑60s R=" + F4(rDecayed) +
                      std::string(releases ? "(继续掉)" : "(没掉!)") + "; ";
        }
        h.Req("case6", "取消（Esc 或点到别处走同一个 ShutdownCancel）：0/1/2 击三档都能取消，"
                       "进度归零、R_d 归零、R 交回时间自己衰减",
              detail, ok);
    }

    // ---- 8) 抖动：帧号的纯函数、幅度随档位递增、进入段是"从猛收到常态" ----
    {
        bool deterministic = true;
        for (int clicks = 1; clicks <= 3; ++clicks) {
            for (int k = 0; k < 240; ++k) {
                if (dshb::ShutdownJitterAt(k, clicks) != dshb::ShutdownJitterAt(k, clicks)) deterministic = false;
            }
        }
        std::string detail;
        bool increasing = true;
        double prevPk = -1.0;
        for (int clicks = 1; clicks <= 3; ++clicks) {
            std::vector<double> v;
            for (int k = 0; k < 240; ++k) v.push_back(dshb::ShutdownJitterAt(k, clicks));
            const double pk = PeakToPeak(v);
            if (pk <= prevPk) increasing = false;
            prevPk = pk;
            detail += "档" + std::to_string(clicks) + " 峰峰值=" + F4(pk) + "DIP; ";
            if (verbose) {
                std::string row = "  档" + std::to_string(clicks) + " k=0..11: ";
                for (int k = 0; k < 12; ++k) row += F4(dshb::ShutdownJitterAt(k, clicks)) + " ";
                std::printf("%s\n", row.c_str());
            }
        }
        h.Req("case7", "抖动是**帧号的纯函数**（同 k 同档两次调用逐位相同）且幅度随档位严格递增",
              "确定性=" + std::string(deterministic ? "yes" : "no") + " " + detail +
                  "（档位=已点几下；tier3 最猛）",
              deterministic && increasing);
    }

    // ---- 9) 进入段：进度是帧号的纯函数，7 帧走完；曲线层在进入段里还画，之后不画 ----
    {
        dshb::DisplayedAmount c;
        RunSeconds(&c, 0, 10);
        const bool enteredNow = dshb::ShutdownEnter();
        const double p0 = dshb::ShutdownEntryProgress(0);
        const double p3 = dshb::ShutdownEntryProgress(3);
        const double p7 = dshb::ShutdownEntryProgress(dshb::kShutdownEntryFrames);
        const double p99 = dshb::ShutdownEntryProgress(200);
        const bool layerIn = dshb::ShutdownCurveLayerVisible();     // 进入那一帧（frame = -1）
        // 进入段是 frame 0..6 这 7 帧；到 frame 7 才不画。所以要多推一帧才看到"整层不画"。
        for (int k = 0; k <= dshb::kShutdownEntryFrames; ++k) c.Update(1.0 / 60.0);
        const bool layerOut = dshb::ShutdownCurveLayerVisible();    // 进入段走完
        h.Req("case8", "进入段：进度 0 -> 3/7 -> 1（7 帧 = 0.117 s ≈ 设计的 0.12 s）；"
                       "曲线层进入段里画、之后不画",
              "p(0)=" + F4(p0) + " p(3)=" + F4(p3) + " p(7)=" + F4(p7) + " p(200)=" + F4(p99) +
                  " 曲线层 进入帧=" + std::string(layerIn ? "画" : "不画") + " 走完=" +
                  std::string(layerOut ? "画" : "不画") + "（frame=" +
                  std::to_string(dshb::ShutdownFrame()) + "）",
              enteredNow && p0 == 0.0 && std::fabs(p3 - 3.0 / 7.0) < 1e-12 && p7 == 1.0 && p99 == 1.0 &&
                  layerIn && !layerOut);
        dshb::ShutdownCancel();
    }

    // ---- 10) 第 3 击：发信号，此后不再受理、不重复触发（放在最后：它钉住 Fired）----
    // ★ 先取状态再试着动它：三下之后每一次输入都必须被拒，且状态**一位都不许变**
    //   （少了 Fired 那道闸，一次 Esc 就能把该关的挂件留在屏幕上）。
    dshb::ShutdownEnter();
    dshb::ShutdownClick();
    dshb::ShutdownClick();
    const bool fired3 = dshb::ShutdownClick();
    const bool firedState = dshb::ShutdownFired();
    const int clicksAfterFired = dshb::ShutdownClicks();
    const bool clickAfterFired = dshb::ShutdownClick();
    const bool cancelAfterFired = dshb::ShutdownCancel();
    const bool enterAfterFired = dshb::ShutdownEnter();
    h.Req("case5", "第 3 击：ShutdownFired=yes；再点、再取消、再进入**都不受理**（不重复触发、不产生第二个实例）",
          "fired3=" + std::string(fired3 ? "yes" : "no") + " ShutdownFired=" +
              std::string(firedState ? "yes" : "no") + " 点满后 clicks=" + std::to_string(clicksAfterFired) +
              " 再点返回=" + std::string(clickAfterFired ? "true" : "false") + " 再取消返回=" +
              std::string(cancelAfterFired ? "true" : "false") + " 再进入返回=" +
              std::string(enterAfterFired ? "true" : "false") + " 之后 clicks=" +
              std::to_string(dshb::ShutdownClicks()) + " ShutdownFired=" +
              std::string(dshb::ShutdownFired() ? "yes" : "no") + " R_d=" +
              F4(dshb::ShutdownFloorRatio()),
          fired3 && firedState && clicksAfterFired == 3 && !clickAfterFired && !cancelAfterFired &&
              !enterAfterFired && dshb::ShutdownFired() && dshb::ShutdownClicks() == 3 &&
              std::fabs(dshb::ShutdownFloorRatio() - dshb::kShutdownRd2) < 1e-12);

    // ---- 11) 托盘菜单「关闭」那个口子：不是"第一击"，而是"直接到第三击" ----
    // ★ 为什么这一条在 case5 之后、而且只量"已经在关闭态"那一半：Fired 是**单向门**
    //   （取消 / 再进入 / 再点击一律不受理，case5 量的就是它），而 ShutdownFireNow 的每一条路
    //   都要走到 Fired —— 一个进程里只走得了一次。所以"从 Off 出发"那一半由 trayprobe 在
    //   **新进程**里量（在那里它正好就是菜单路径：用户点托盘「关闭」时状态天然是 Off）。
    //   这里量另一半：已经在关闭态里再点托盘「关闭」= "我改主意了，现在就关"。
    // ★ Armed 那一档只能用导帧夹具摆出来：Fired 回不去 Off，而 ShutdownEnter 在 Fired 里
    //   不受理。夹具写的正是这个状态机自己的三个字段（clicks/phase/frame），而 ShutdownFireNow
    //   只读其中两个 —— 所以它是"输入"，不是另一套实现。
    {
        const double rdBefore = dshb::ShutdownFloorRatio();
        dshb::SetShutdownFixture(1, 0);          // Armed、clicks=1（= 窗口上点过 1 下）
        const bool armed = dshb::ShutdownActive() && !dshb::ShutdownFired() &&
                           dshb::ShutdownClicks() == 1;
        const bool firedNow = dshb::ShutdownFireNow();
        const int clicksNow = dshb::ShutdownClicks();
        const double rdNow = dshb::ShutdownFloorRatio();
        const double glowNow = dshb::ShutdownGlowLevel();
        // 一次性到 Fired 之后：不重复触发，四种输入一位都不许改状态
        const bool fireAgain = dshb::ShutdownFireNow();
        const bool clickAfter = dshb::ShutdownClick();
        const bool cancelAfter = dshb::ShutdownCancel();
        const bool enterAfter = dshb::ShutdownEnter();
        h.Req("case9",
              "托盘「关闭」那个口子（ShutdownFireNow）：已在关闭态（Armed、已点 1 下）时"
              "**一次调用**就到第三击的终点状态（clicks=3 / Fired / R_d=1.00 / 亮度按第 2 击那一档），"
              "此后不重复触发、也不受理任何输入",
              "夹具摆出的起点: active=yes fired=no clicks=1 R_d=" + F4(rdBefore) + "（= case5 留下的"
              " Fired 之后用夹具摆回来的，Fired 回不去 Off）; FireNow=" +
                  std::string(firedNow ? "yes" : "no") + " -> clicks=" + std::to_string(clicksNow) +
                  " fired=" + std::string(dshb::ShutdownFired() ? "yes" : "no") + " R_d=" + F4(rdNow) +
                  " 亮度倍率=" + F4(glowNow) + "（第 2 击及以后那一档 = " +
                  F4(dshb::kShutdownGlowLevel2) + "）; 再 FireNow=" +
                  std::string(fireAgain ? "true" : "false") + " 再点击=" +
                  std::string(clickAfter ? "true" : "false") + " 再取消=" +
                  std::string(cancelAfter ? "true" : "false") + " 再进入=" +
                  std::string(enterAfter ? "true" : "false") + " -> clicks=" +
                  std::to_string(dshb::ShutdownClicks()) + " fired=" +
                  std::string(dshb::ShutdownFired() ? "yes" : "no") + " R_d=" +
                  F4(dshb::ShutdownFloorRatio()) + "（与 case5 三击那条**同一个终点状态**）",
              armed && firedNow && dshb::ShutdownFired() && clicksNow == 3 &&
                  std::fabs(rdNow - dshb::kShutdownRd2) < 1e-12 &&
                  std::fabs(glowNow - dshb::kShutdownGlowLevel2) < 1e-12 && !fireAgain &&
                  !clickAfter && !cancelAfter && !enterAfter && dshb::ShutdownClicks() == 3 &&
                  dshb::ShutdownFired() &&
                  std::fabs(dshb::ShutdownFloorRatio() - dshb::kShutdownRd2) < 1e-12);
    }

    std::printf("checks: %d passed, %d failed\n", h.passed, h.failed);
    if (!h.failures.empty()) {
        std::printf("FAILURES:\n");
        for (const std::string& f : h.failures) std::printf("  %s\n", f.c_str());
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}
