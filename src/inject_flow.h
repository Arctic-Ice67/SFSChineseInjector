// 注入流程：CLI 和界面共用。资源优先从 exe 内嵌载荷取，其次 data 目录（开发时）。
#pragma once

#include <cstdint>
#include <string>

#include "game.h"

namespace inject_flow {

// 本体汉化文本（载荷优先）
std::string TranslationText();

// 合并后的词表 JSON（载荷 dict.json 优先，否则现场合并 data\catalog）
bool DictJson(std::string& out, std::string& err);

// 等游戏进程起来（最多 wait_ms 毫秒）然后纯内存注入。全程不落盘（字体临时文件用完即删）
bool Run(const game::Layout& layout, uint32_t wait_ms, std::string& err);

}  // namespace inject_flow
