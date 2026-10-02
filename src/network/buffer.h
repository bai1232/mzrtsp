/*
 * Buffer：线性可扩容字节缓冲（M2-3a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M2.md §3.3（本文件的接口就是那一节；形变必须同批同步）
 *
 * 为什么是"线性数组 + 读游标"，而不是环形缓冲 / 链式零拷贝切片：
 *   - 环形缓冲：写满要处理回绕，find() 跨尾部分两段查 —— 代码量翻倍，只为省一次 memmove；
 *   - 链式切片（iovec 列表）：适合"只看不改地转发"的代理；本项目 HTTP 头解析、FLV 封装
 *     都要线性访问，链式会让每次访问都退化成遍历。
 *   线性 + compact 的代价是偶发 memmove，换来的是所有访问都是裸指针。
 *
 * 不变量：0 <= _read_pos <= _write_pos <= _buf.size()，size() == _write_pos - _read_pos
 *
 * 返回值策略（AI_COLLAB：尽量不用 void）：
 *   本类**没有** void 返回的公开接口 —— 写/消费返回实际完成量，清空/释放返回被处理的
 *   字节数。理由：调用方永远能判断"我要求的"和"实际发生的"差多少；void 会把这个信息
 *   抹掉，只能靠"约定它一定全部完成"来推理（而约定一旦被破坏就是静默错位）。
 *
 * 线程模型：**不线程安全**，只在 poller 线程使用（DESIGN_M2 §5）。
 * 容量策略：**Buffer 自己不设上限**（容器职责单一）；"多大算攻击"是协议层策略，
 *           由 Session 的接收缓冲上限负责（DESIGN_M2 §3.3 §4.6）。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <sys/types.h>

namespace mzmedia {

class Buffer {
public:
    using Ptr = std::shared_ptr<Buffer>;
    static constexpr size_t npos = static_cast<size_t>(-1);

    /// @param initial_capacity 预分配容量（0 = 首次 append 时再分配）
    explicit Buffer(size_t initial_capacity = 0);
    ~Buffer();
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept;
    Buffer &operator=(Buffer &&other) noexcept;

    // ------------------------------------------------------------------
    // 访问
    // ------------------------------------------------------------------

    /// 可读区首地址；**追加 / compact / reserve 之后会失效**，不要跨写操作保存
    const char *data() const;
    size_t size() const;
    size_t capacity() const;
    bool empty() const;
    /// 读游标（已消费字节数；诊断"compact 有没有生效"用）
    size_t readPos() const;

    std::string toString() const;

    // ------------------------------------------------------------------
    // 写入 / 容量
    // ------------------------------------------------------------------

    /**
     * 追加数据
     * @return 实际写入的字节数：正常 == len；data == nullptr 或 len == 0 时 == 0
     * @note 没有容量上限，因此**要么全部写入、要么抛 std::bad_alloc**：
     *       不做"写不下就截断"（静默截断 = 数据错位，AI_COLLAB §4.5）
     */
    size_t append(const void *data, size_t len);
    size_t append(const std::string &str);

    /// 预分配容量
    /// @return 处理后的**实际容量**（≥ 请求值；容量已经够大时原样返回）
    size_t reserve(size_t capacity);

    // ------------------------------------------------------------------
    // 消费
    // ------------------------------------------------------------------

    /**
     * 消费（丢弃）头部 len 字节
     * @return 实际消费的字节数：len <= size() 时 == len；否则 **== 0 且不改动任何数据**
     * @note len > size() 属于调用方的解析逻辑算错了：这里 ErrorP + 拒绝执行，
     *       而不是"按 size() 截断"。截断会让"少读了几个字节"变成后续一直错位、
     *       极难定位的 bug（AI_COLLAB §4.5）。协议层标准写法：
     *       `if (buf->consume(n) != n) { 需要更多数据 }`
     */
    size_t consume(size_t len);

    /// 丢弃全部数据、**容量保留**（长连接反复收发不要反复 malloc）
    /// @return 被丢弃的字节数（即调用前的 size()）
    size_t clear();

    /// 连容量一起释放（应对"突发大包之后长期空闲"）
    /// @return 被释放的容量字节数
    size_t release();

    // ------------------------------------------------------------------
    // 查找（返回偏移而不是指针：扩容后 data() 会失效，偏移天然安全）
    // ------------------------------------------------------------------

    /// @return 找到返回偏移；否则 npos
    size_t find(char ch, size_t from = 0) const;
    size_t find(const void *needle, size_t needle_len, size_t from = 0) const;
    bool startWith(const void *prefix, size_t len) const;
    bool endWith(const void *suffix, size_t len) const;

    // ------------------------------------------------------------------
    // fd 读写
    // ------------------------------------------------------------------

    /**
     * 循环 read() 追加到本缓冲，直到 EAGAIN / EOF / 出错 / 达到 max_bytes
     *
     * @param max_bytes **必填**：本次最多新增多少字节。ET 模式下"读到 EAGAIN"是硬要求，
     *                  但没有上限的读循环等于把内存交给对端控制（AI_COLLAB §4.3）；
     *                  §3.3 已决定"上限策略在 Session"，原签名却让 Session 无法在循环
     *                  中途设限 —— 所以上限必须作为参数进来。
     * @param hit_limit 非空时写出"是否因为达到 max_bytes 才停下"
     * @param err       非空时写出 errno（仅 -1 时有意义）
     * @return >0 新增字节数；0 = 没有更多数据（EAGAIN）或对端关闭（EOF）；
     *         -1 = 真错误（*err 带 errno）或非法调用（max_bytes == 0）
     * @warning hit_limit == true 表示 **socket 里可能还有数据而本次没读完**。
     *          ET 模式下不会再有读事件通知，调用方必须二选一：
     *          ① 继续读（放大 max_bytes 再来一次）；② 按超限断开。
     *          **绝不能当作"读完了"忽略**，否则连接永久假死（DESIGN_M2 §8 R1）。
     */
    /// @param eof 写出"对端是否已关闭"：**必须与 EAGAIN 分开**（EAGAIN 要继续等、
    ///            EOF 要关连接；两者都返回 0，不给标志就分不出来）
    ssize_t readFromFd(int fd, size_t max_bytes, bool *eof = nullptr, bool *hit_limit = nullptr,
                       int *err = nullptr);

    /**
     * 尽量写出可读区（部分写时消费已写出的部分）
     * @return >0 本次写出字节数（可能 < size()，剩余仍留在缓冲里，等 EPOLLOUT）；
     *         0 = 一个字节都没写出去（EAGAIN，不是错误）；-1 = 真错误（*err 带 errno）
     * @note 空缓冲时返回 0（调用方不该在空的时候调它，但也不能崩）
     */
    ssize_t writeToFd(int fd, int *err = nullptr);

private:
    /// 把剩余数据搬到头部（读游标前移后回收前导空间）；调用方保证 _read_pos > 0
    void compact();
    /// 保证可写区至少还能放 want 字节（必要时 compact + 扩容）
    void ensureWritable(size_t want);

    std::string _buf;       // 底层存储：data() == _buf.data() + _read_pos
    size_t _read_pos = 0;   // 已消费到的位置
    size_t _write_pos = 0;  // 已写入到的位置
};

} // namespace mzmedia
