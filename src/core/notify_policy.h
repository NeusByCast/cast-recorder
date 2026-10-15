#pragma once
// Where a "recording started / stopped / saved" or "replay saved" notice goes (v1.0 QA feedback):
//  - Cast Recorder is open, not minimized and in front -> nowhere: the in-app status line already says it
//    (no toast, no popup, no duplicate in-app card).
//  - minimized, hidden, or another app's window is in front -> one on-screen toast (excluded from capture).
//  - never a Windows system notification.
namespace cr {
enum class NoticeTarget { StatusOnly, OnScreenToast };
inline NoticeTarget noticeTarget(bool windowVisible, bool minimized, bool foregroundIsOurs) {
    const bool appInFront = windowVisible && !minimized && foregroundIsOurs;
    return appInFront ? NoticeTarget::StatusOnly : NoticeTarget::OnScreenToast;
}
}  // namespace cr
