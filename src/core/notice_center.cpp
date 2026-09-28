#include "core/notice_center.h"

#include <algorithm>

namespace mzmedia {

NoticeCenter &NoticeCenter::Instance() {
    // 故意泄漏：避免静态析构顺序问题（与 Logger 一致，理由见头文件注释）
    static NoticeCenter *instance = new NoticeCenter();
    return *instance;
}

std::vector<NoticeCenter::Entry> NoticeCenter::snapshot(const std::string &event) const {
    std::lock_guard<std::mutex> lck(_mtx);
    const auto it = _listeners.find(event);
    if (it == _listeners.end()) {
        return {};
    }
    // 只拷贝 shared_ptr（廉价），保证回调期间即使监听器被注销，对象也还活着
    return it->second;
}

void NoticeCenter::delListener(void *tag, const std::string &event) {
    const auto matches = [tag](const Entry &entry) { return entry.tag == tag; };

    std::lock_guard<std::mutex> lck(_mtx);

    if (!event.empty()) {
        const auto it = _listeners.find(event);
        if (it == _listeners.end()) {
            return;
        }
        auto &entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(), matches), entries.end());
        if (entries.empty()) {
            _listeners.erase(it);
        }
        return;
    }

    // 未指定事件：清理该 tag 在所有事件上的监听
    for (auto it = _listeners.begin(); it != _listeners.end();) {
        auto &entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(), matches), entries.end());
        if (entries.empty()) {
            it = _listeners.erase(it);
        } else {
            ++it;
        }
    }
}

size_t NoticeCenter::listenerCount(const std::string &event) const {
    std::lock_guard<std::mutex> lck(_mtx);
    const auto it = _listeners.find(event);
    return it == _listeners.end() ? 0 : it->second.size();
}

size_t NoticeCenter::listenerCount() const {
    std::lock_guard<std::mutex> lck(_mtx);
    size_t total = 0;
    for (const auto &item : _listeners) {
        total += item.second.size();
    }
    return total;
}

void NoticeCenter::clear() {
    std::lock_guard<std::mutex> lck(_mtx);
    _listeners.clear();
}

} // namespace mzmedia
