#pragma once

#include <cstdint>

namespace mirrorui {

/**
 * 按进程 PID 控制其默认渲染音频会话的静音状态(WASAPI)。
 * @return true 表示成功找到并设置了该进程的音频会话
 */
bool setProcessAudioMute(uint64_t processId, bool mute);

} // namespace mirrorui
