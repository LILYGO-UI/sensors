# 实体键盘环境下的启动闪退（2026-09-29）

## 原因与证据

Sensors 在 launcher 中启动后约 180–250 ms 收到 `SIGSEGV`，退出码为
139。目标板上的 `pi` 用户具备 I2C、音频、显示和输入设备访问权限，
无界面采集能够运行。捕获到的主线程调用栈为：

```text
focus_next_core
lv_group_remove_obj
hide_software_keyboards
input_housekeeping
cm0_app_run
```

AppKit 的 `hide_software_keyboards()` 在遍历对象树时，无条件将每个
子对象移出焦点组，误伤了普通按钮、选项卡按钮矩阵和下拉框。
移除当前焦点会同步触发下一个可编辑控件的 `LV_EVENT_FOCUSED`，
运行时的焦点回调再次调用同一个遍历函数。嵌套调用清空焦点组后，
外层 LVGL `focus_next_core()` 解引用空的 `obj_focus`，导致崩溃。

只需普通按钮和按钮矩阵就能复现，不依赖软件键盘、传感器设备或
root 身份。这次故障来自硬件键盘适配中的焦点管理代码，与应用降权无关。

## 修复

AppKit 现在仅将实际的 `lv_keyboard` 对象移出焦点组，搜索过程中保留
其他控件。LVGL 软件键盘的键由单个按钮矩阵表示，不是独立子对象。

修复保存在独立 AppKit 仓库的 `src/runtime.cpp`，同时应用到本项目
`third_party/cm0-appkit/src/runtime.cpp`。AppKit 的
`tests/test_runtime_input.cpp` 使用真实 LVGL 检查普通控件、焦点回调
重入和软件键盘隐藏/焦点交接。

AppKit 静态链接到应用中，因此更新源 SDK 后必须重编译受影响的应用；
单独更新板上的 SDK 包不会修复已安装的应用二进制。

## 验证结果

- 旧代码：新增的三项回归测试全部失败，焦点测试稳定触发段错误；完整
  Sensors 主机程序使用 SDL dummy 驱动同样以 `SIGSEGV` 退出。
- 修复后：AppKit 全部 12 项测试、Sensors 全部 9 项测试通过。
- 完整 Sensors 主机程序在 568×1232、1232×568 下各运行 4 秒，均输出
  `UI ready`，收到 `SIGTERM` 后以 0 退出。
- 目标板：以 `pi`（UID 1000）运行，确认打开 Goodix 触摸屏和 TCA8418
  键盘；横竖屏各持续运行 8 秒，首帧分别为 159 ms、181 ms，正常退出。
  验证后恢复 launcher 服务。
- ARM64 修复包已安装到目标板。安装后校验的二进制 SHA-256 为
  `d559e50e7c3a1bfa7e9b4b540e8bd3eab03ea285e3657fd4c4867589879978ca`。
- 安装后用户从 launcher 再次打开 Sensors，确认可以正常打开。

上述限时检查覆盖本次启动崩溃和焦点回归，不代表所有传感器页面已经
完成长时间交互验证。
