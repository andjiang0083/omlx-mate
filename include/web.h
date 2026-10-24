#pragma once
void webBegin();
void webLoop();
void webStop();                  // 熄屏：停服务器（唤醒后 webBegin 重建）
bool webIsRunning();
bool agentPullSnapshot();        // 唤醒后主动拉一次快照（设备侧 pull，不依赖推送）
