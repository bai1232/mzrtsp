/*
 * live_demo：一路直播流的完整生命周期（最小可运行示例）
 * ============================================================================
 * 用法：./build/bin/live_demo [输入文件] [输出目录]
 *   默认：samples/sample.mp4  →  /tmp/mzmedia_live_demo/{client1,client2}.flv
 *
 * 它把项目里**真实的那条链**串起来（不是 mock、不是简化版）：
 *
 *     Demuxer（解封装）
 *        └─ DemuxerProducer（AVPacket → MediaPacket；Annex-B/ADTS 的形状转换）
 *             └─ SourcePump（源线程：按墙钟节流，逐包 push）
 *                  └─ MediaSource（分发中心：GOP 缓存 + 每订阅者队列 + 唤醒）
 *                       ├─ Subscriber A（FrameQueue，绑定 poller）
 *                       │    └─ FlvSender A（内部 FlvMuxer：MediaPacket → FLV tag 字节）
 *                       └─ Subscriber B
 *                            └─ FlvSender B
 *
 * 与 `src/main.cpp` 的差别只有一处：**ByteSink 写文件而不是写 socket**。
 * 真实服务里 FlvSender 的 Sink 由 `HttpResponse::chunkWriter()` 提供（见 main.cpp 的 /live/ 路由），
 * 这里换成 ofstream 是为了让你能在本机把两个客户端的字节落盘、用 ffprobe 独立验证。
 *
 * 分五个阶段（见下面的 "阶段" 注释），每段都打印可观测数字。
 * ============================================================================
 */

#include "mzmedia.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

/**
 * 一个"客户端"的量。
 * 为什么用 shared_ptr 而不是成员：`FlvSender::Sink` 里的 lambda 会**活到 sender 析构为止**，
 * 如果它们按引用捕获栈上的计数器/文件对象，就会出现"对象先死、lambda 后跑"的 UAF。
 * 按值捕获 shared_ptr 是最省心的做法（ASAN 跑一遍就能验证这条）。
 */
struct ClientStats {
    std::atomic<uint64_t> bytes{0};   ///< 落盘字节数（= 这个客户端收到的 FLV 字节）
    std::atomic<uint64_t> writes{0};  ///< sink.write 被调用的次数（FlvSender 按 32KB 攒块）
    std::atomic<bool> ended{false};   ///< sink.end 被调用过（流正常结束）
    std::atomic<bool> aborted{false}; ///< sink.abort 被调用过（异常终止）
};

struct Client {
    std::string name;
    std::string path;
    std::shared_ptr<std::ofstream> file;
    std::shared_ptr<ClientStats> stats;
    FlvSender::Ptr sender;         ///< 每客户端一份（内部持有 Subscriber + FlvMuxer）
    Subscriber::Id subscriber_id = 0;
};

/// 把一个客户端接进来：订阅 → 绑 poller → 建 FlvSender（写文件）→ 立刻发 FLV 头
/// @note `start()` 走 `poller->sync(...)`：它内部会操作订阅者队列，必须与该队列的消费者同线程
bool attachClient(Client *client, const EventPoller::Ptr &poller, const MediaSource::Ptr &source,
                  DemuxerProducer &producer) {
    client->file = std::make_shared<std::ofstream>(client->path, std::ios::binary | std::ios::trunc);
    if (!client->file->is_open()) {
        std::printf("  ✘ 打不开输出文件：%s\n", client->path.c_str());
        return false;
    }
    client->stats = std::make_shared<ClientStats>();

    // ① 订阅：MediaSource 会把**当前 GOP 缓存**灌进这个新队列（保证第一包是视频关键帧）
    Subscriber::Ptr subscriber = source->subscribe();
    if (!subscriber) {
        std::printf("  ✘ 订阅失败（人数上限 %zu 或关键帧放不进队列）\n",
                    source->limits().max_subscribers);
        return false;
    }
    client->subscriber_id = subscriber->id();
    // ② 绑定消费者线程：之后"有数据了"的唤醒会投递到这个 poller 线程上执行 drain 回调
    (void) subscriber->bindPoller(poller);

    // ③ Sink：FlvSender 只认"写出去"这一个动作 —— 这里落到文件（真实服务里是 chunked 写 socket）
    FlvSender::Sink sink;
    auto file = client->file;
    auto stats = client->stats;
    sink.write = [file, stats](const char *data, size_t len) -> bool {
        file->write(data, static_cast<std::streamsize>(len));
        stats->bytes.fetch_add(len);
        stats->writes.fetch_add(1);
        return file->good(); // false = 连接已坏，FlvSender 会以 SinkFailed 终止
    };
    sink.end = [stats]() { stats->ended.store(true); };
    sink.abort = [stats]() { stats->aborted.store(true); };

    // ④ 建 sender：它会在订阅者上注册自己的 drain 回调（FlvSender::create 内部做的）
    client->sender = FlvSender::create(
        sink, FlvMuxer::Streams{producer.muxerVideo(), producer.muxerAudio()}, std::move(subscriber));
    if (!client->sender) {
        std::printf("  ✘ FlvSender::create 失败（这路流封不了 FLV：需要 H264(avcC) + AAC(ASC)）\n");
        return false;
    }

    // ⑤ 发 FLV Header + AVC/AAC sequence header（FR-3.1 的"立即"，不等第一帧），并立刻搬一次队列
    FlvSender::Result started = FlvSender::Result::NotStarted;
    try {
        poller->sync([&client, &started]() { started = client->sender->start(); });
    } catch (const std::exception &e) {
        std::printf("  ✘ 投递 start() 失败：%s\n", e.what());
        return false;
    }
    if (started != FlvSender::Result::Ok) {
        std::printf("  ✘ FlvSender::start 失败：%s（%s）\n", FlvSender::resultName(started),
                    client->sender->lastError().c_str());
        return false;
    }
    std::printf("  ✔ [%s] 已接入：subscriber id=%llu，FLV 头 + sequence header 已发出（%llu 字节落盘）\n",
                client->name.c_str(), static_cast<unsigned long long>(client->subscriber_id),
                static_cast<unsigned long long>(client->stats->bytes.load()));
    return true;
}

/// 在 **poller 线程**里销毁 sender（= 退订 + 释放 FlvMuxer）
/// @note 这就是"跨线程销毁必须投递回 poller"的演示：若在主线程 reset，
///       poller 可能正拿着这个订阅者做 drain → UAF。真实服务里由连接/Session 的
///       `holdResource` 在 poller 线程上释放（见 src/http/http_server.cpp 的 HttpSession 析构）。
void detachClient(Client *client, const EventPoller::Ptr &poller) {
    if (!client->sender) {
        return;
    }
    try {
        poller->sync([client]() {
            client->sender.reset(); // 析构 → Subscriber 引用计数归零 → 源侧惰性注销
        });
    } catch (const std::exception &e) {
        std::printf("  ⚠ 投递 detach 失败（%s）→ 就地释放（仅演示可接受）\n", e.what());
        client->sender.reset();
    }
    if (client->file) {
        client->file->flush();
        client->file->close();
    }
    std::printf("  ✔ [%s] 已断开：订阅者注销（源侧惰性清理），落盘 %llu 字节 / %llu 次写\n",
                client->name.c_str(), static_cast<unsigned long long>(client->stats->bytes.load()),
                static_cast<unsigned long long>(client->stats->writes.load()));
}

/// 轮询等待某个条件成立（演示里只用来等"数据到了"，不参与任何判据）
template <typename F>
bool waitUntil(F cond, int timeout_ms) {
    const uint64_t deadline = getCurrentMillisecond() + static_cast<uint64_t>(timeout_ms);
    while (getCurrentMillisecond() < deadline) {
        if (cond()) {
            return true;
        }
        sleepMs(10);
    }
    return cond();
}

} // namespace

int main(int argc, char **argv) {
    const std::string input = (argc > 1) ? argv[1] : "samples/sample.mp4";
    const std::string out_dir = (argc > 2) ? argv[2] : "/tmp/mzmedia_live_demo";

    std::printf("live_demo：一路直播流的完整生命周期\n");
    std::printf("  输入：%s\n  输出目录：%s\n", input.c_str(), out_dir.c_str());
    if (!createDirectory(out_dir)) {
        std::printf("✘ 创建输出目录失败：%s\n", out_dir.c_str());
        return 1;
    }

    // =====================================================================
    // 阶段① 起源：打开源 → 造 producer（解封装）→ 造 source（分发中心）
    // =====================================================================
    std::printf("\n=== 阶段① 起源 ===\n");
    // EventPoller：一个线程 + 一个 epoll。所有"投递到消费者线程"的唤醒都走它。
    EventPoller::Ptr poller = EventPoller::create("live-demo");

    // 解封装配置：loop=true（2 秒的样本会一遍遍重放，时间戳接着往后加）。
    // 为什么要 loop：样本只有 1 个关键帧（dts=0），不 loop 的话"中途接入"永远只能拿到 dts=0 的 GOP；
    // loop 之后第 2 趟的关键帧在 dts=2000，中途接入就能看到"首帧源 dts 更晚"的效果。
    DemuxerProducer::Config producer_config;
    producer_config.loop = true;
    auto producer = std::make_shared<DemuxerProducer>(producer_config);
    if (!producer->open(input)) {
        std::printf("✘ 打开失败：%s\n", producer->lastError().c_str());
        return 1;
    }
    const StreamInfo *video = producer->muxerVideo();
    const StreamInfo *audio = producer->muxerAudio();
    std::printf("  ✔ Demuxer 已打开：%s\n", input.c_str());
    std::printf("     视频：%s %dx%d，extradata(avcC) %zu 字节%s\n",
                video != nullptr ? video->codec_name.c_str() : "(无)",
                video != nullptr ? video->width : 0, video != nullptr ? video->height : 0,
                (video != nullptr && video->extradata) ? video->extradata->size() : 0u,
                producer->annexBVideo() ? "（Annex-B → 已现场构造 avcC）" : "");
    std::printf("     音频：%s，extradata(ASC) %zu 字节%s\n",
                audio != nullptr ? audio->codec_name.c_str() : "(无)",
                (audio != nullptr && audio->extradata) ? audio->extradata->size() : 0u,
                producer->adtsAudio() ? "（ADTS → 已现场构造 ASC）" : "");

    // 分发中心：一源一份输出。上限**由物理量推导**（码率 × 延迟额度），不是拍脑袋的字节数。
    auto source = std::make_shared<MediaSource>();
    const MediaSource::Limits limits = source->limits();
    std::printf("  ✔ MediaSource 已创建：每订阅者队列上限 %zu 字节（%u kbps × %u ms）、"
                "GOP 缓存上限 %zu 字节、最多 %zu 个订阅者\n",
                limits.queueMaxBytes(), static_cast<unsigned>(limits.max_bitrate_bps / 1000),
                limits.latency_budget_ms, limits.gopMaxBytes(), limits.max_subscribers);

    // 源线程：SourcePump 拿 producer->read 当读回调，按墙钟节流后逐包 push 给 source。
    auto pump = std::make_shared<SourcePump>();
    Throttle::Config throttle;
    throttle.enabled = true;
    throttle.speed = 2.0; // 2 倍速：2 秒样本约 1 秒推完，演示跑得快一点
    if (!pump->setThrottleConfig(throttle)) {
        std::printf("✘ 节流配置非法\n");
        return 1;
    }
    // 把 pump 的停止标志接到 Demuxer 的 interrupt_callback：stop() 才能真正打断 av_read_frame
    (void) producer->setAbortFlag(&pump->stopFlag());
    std::printf("  ✔ SourcePump 就绪（节流 %gx）、DemuxerProducer 已绑定中止标志\n", throttle.speed);

    // =====================================================================
    // 阶段② 订阅：客户端 A 从 0 开始；客户端 B **中途接入**（验证从关键帧起播）
    // =====================================================================
    std::printf("\n=== 阶段② 订阅 ===\n");
    Client client1;
    client1.name = "client1";
    client1.path = out_dir + "/client1.flv";
    if (!attachClient(&client1, poller, source, *producer)) {
        return 1;
    }

    // 起源线程：读到包就 push。这就是 `DemuxerProducer::read` 作为 `SourcePump::ReadFn` 的用法。
    const bool started = pump->start(
        [producer](MediaPacket::Ptr *packet, std::string *error) {
            return producer->read(packet, error); // 返回 Packet / EndOfStream / Error
        },
        source);
    if (!started) {
        std::printf("✘ 源线程启动失败（已在运行 / 参数为空）\n");
        return 1;
    }
    // 等"第 2 趟"开始（producer->loopCount() >= 1）再接入 client2：
    // 这样 GOP 缓存里装的是第 2 趟的关键帧（源 dts ≈ 2000ms），"中途接入"才有对比意义。
    // （样本只有 1 个关键帧；若不 loop，任何时刻的 GOP 缓存都从 dts=0 开始。）
    std::printf("  ✔ 源线程已启动，等待第 2 趟开始（producer->loopCount() >= 1）后接入 client2\n");
    (void) waitUntil([&producer]() { return producer->loopCount() >= 1; }, 4000);
    sleepMs(100); // 让第 2 趟的关键帧真正进入 GOP 缓存

    // 客户端 B **中途接入**。它拿到的第一包来自 GOP 缓存（保证关键帧），源 dts 通常已经 > 0。
    // 注意：FlvMuxer 是**每客户端一份**，时间戳基准取"该客户端的第一个包" → 两个人的 FLV 都从 0 起。
    const std::vector<MediaPacket::Ptr> gop = source->gopCache().snapshot();
    Client client2;
    client2.name = "client2";
    client2.path = out_dir + "/client2.flv";
    if (!attachClient(&client2, poller, source, *producer)) {
        return 1;
    }
    if (!gop.empty()) {
        const MediaPacket::Ptr &first = gop.front();
        size_t gop_bytes = 0;
        for (const auto &packet : gop) {
            gop_bytes += packet->size();
        }
        std::printf("  · client2 的首包来自 GOP 缓存：源 dts=%lld ms，关键帧=%s，GOP 缓存 %zu 包 / %zu 字节\n",
                    static_cast<long long>(first->dtsMs()), first->isKeyFrame() ? "是" : "否",
                    gop.size(), gop_bytes);
        std::printf("  · 对比：client1 的首包是源的第 1 个关键帧（源 dts=0），client2 从 dts=%lld ms 起播\n",
                    static_cast<long long>(first->dtsMs()));
        std::printf("    —— 但**两条 FLV 文件都从时间戳 0 开始**（每客户端一份 FlvMuxer 基准）\n");
    }

    // =====================================================================
    // 阶段③ 分发：跑几秒，两个订阅者各自把字节落盘
    //   · 源线程 pushPacket → 每个订阅者队列（有界）；队列满时丢"最旧的一段"（音视频成对），
    //     但**视频关键帧绝不丢**；腾不出空间 → 该订阅者 broken（连接层据此断开）。
    //   · 唤醒：源线程不会跨线程直接写 socket，而是 subscriber.notify() → EventPoller::async()
    //     投递到该订阅者绑定的 poller 线程；**多次唤醒会合并**（pending 标志），
    //     所以 drain 回调被调一次可能取走多个包（stats 里 notify/coalesced/popped 可对账）。
    // =====================================================================
    std::printf("\n=== 阶段③ 分发（约 1.5 秒）===\n");
    for (int tick = 0; tick < 6; ++tick) {
        sleepMs(250);
        std::printf("  t=%4d ms | client1 %6llu B | client2 %6llu B | subscribers=%zu | 源包数=%llu\n",
                    (tick + 1) * 250, static_cast<unsigned long long>(client1.stats->bytes.load()),
                    static_cast<unsigned long long>(client2.stats->bytes.load()),
                    source->subscriberCount(),
                    static_cast<unsigned long long>(producer->totalPackets()));
    }

    // =====================================================================
    // 阶段④ 断开：client1 走人（在 poller 线程里销毁 sender）
    // =====================================================================
    std::printf("\n=== 阶段④ 断开 client1 ===\n");
    std::printf("  断开前：subscribers=%zu\n", source->subscriberCount());
    detachClient(&client1, poller);
    const size_t after_detach = source->subscriberCount(); // 内部会先惰性清理已析构的订阅者
    std::printf("  断开后：subscribers=%zu（应为 1，只剩 client2）\n", after_detach);

    // =====================================================================
    // 阶段⑤ 结束：停源 → 销毁 → 统计
    //   销毁顺序（顺序错了就会 UAF 或 join 卡住）：
    //     1) pump->stop()  —— 先停源线程并 join（此时 producer 必须还活着：读循环还在用它）
    //     2) 各客户端 sender（在 poller 线程里）—— 退订 + 释放 FlvMuxer
    //     3) producer.reset()
    //     4) source.reset() —— 此时 subscribers 必须已经是 0
    //     5) poller->shutdown() —— 最后停事件循环（之前所有 async/sync 都已完成）
    // =====================================================================
    std::printf("\n=== 阶段⑤ 结束 ===\n");
    const bool stopped = pump->stop();
    std::printf("  ✔ SourcePump::stop() → 源线程已 join（stop 返回 %s）\n", stopped ? "true" : "false");
    std::printf("  · 源统计（销毁前）：\n      %s\n", source->dumpStats().c_str());
    std::printf("  · /api/stats 里这一段 JSON（真实服务由 SourceManager 拼进 /api/stats）：\n      {%s}\n",
                source->dumpStatsJson().c_str());

    detachClient(&client2, poller);
    std::printf("  · 所有客户端断开后：subscribers=%zu\n", source->subscriberCount());

    std::weak_ptr<MediaSource> weak_source = source;
    producer.reset();              // 源线程已 join，producer 可以先走
    source.reset();                // 订阅者全走光 → 这里应该真的析构
    poller->shutdown();            // 停事件循环（最后一步）
    const bool source_collected = weak_source.expired();
    std::printf("  ✔ 销毁完成：source 已回收=%s（subscribers 归 0 且 shared_ptr 计数归零）\n",
                source_collected ? "true" : "false");
    std::printf("  ✔ 产物：%s（%llu 字节，ended=%s） / %s（%llu 字节，ended=%s）\n",
                client1.path.c_str(), static_cast<unsigned long long>(client1.stats->bytes.load()),
                client1.stats->ended.load() ? "true" : "false", client2.path.c_str(),
                static_cast<unsigned long long>(client2.stats->bytes.load()),
                client2.stats->ended.load() ? "true" : "false");

    const bool ok = source_collected && client1.stats->bytes.load() > 1000 &&
                    client2.stats->bytes.load() > 1000;
    std::printf("\n%s\n", ok ? "结论：OK（两路都拿到数据、销毁干净）" : "结论：失败（见上面的输出）");
    return ok ? 0 : 1;
}
