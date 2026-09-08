#pragma once

using tjs_int = int;
constexpr tjs_int TVPMaxThreadNum = 8;
using TVP_THREAD_TASK_FUNC = void (*)(void*);
using TVP_THREAD_PARAM = void*;

tjs_int TVPGetThreadNum();
void TVPBeginThreadTask(tjs_int num);
void TVPExecThreadTask(TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param);
void TVPEndThreadTask();
