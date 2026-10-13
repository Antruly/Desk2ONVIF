#pragma once

/**
 * 运行期能读到的软件版本号。
 *
 * ## 为什么是一个编译期宏，而不是运行时读 exe 的 VersionInfo
 *
 * 因为"读自己身上的资源"这条路要 LoadLibraryEx + FindResource 一整套，
 * 而它的用处只有一个：验证版本号有没有漂移。拿验证手段当数据来源，
 * 一旦资源真的没编进去，程序会**安静地**报一个空版本号 —— 而那正是我们要
 * 发现的情况。宏是编译期钉进去的，缺了就是编译错误。
 *
 * ## 唯一的来源
 *
 * `CMakeLists.txt` 的 `project(Desk2ONVIF VERSION x.y.z)` →
 * `target_compile_definitions(... D2O_VERSION="x.y.z")`。
 * 同一份 `PROJECT_VERSION` 还喂给 `resources/desk2onvif.rc.in`
 * （exe 的 VersionInfo）和 `scripts\package.ps1`（包名与 `firmware_version`）。
 * 也就是说这个项目里现在有**四**处会显示版本号，而它们全都从那一行来 ——
 * `scripts\verify_installer.ps1` 与 `verify_portable.ps1` 里各有一条
 * **三方一致**断言盯着（包名 / exe 的 VersionInfo / `/api/status`）。
 *
 * `scripts\package.ps1` 每次打包都会把那一行 +1（用户明确选的规则：
 * 每次打包都自增，`-NoBump` 是"原样重打"的口子）。
 *
 * ## 兜底
 *
 * 用别的方式编译这个头（比如某个只链了 `src/` 的一次性小测试）时没有
 * `D2O_VERSION`，那就报一个**一眼看得出不对**的值 —— 不写兜底的话
 * `D2O_VERSION` 展开成裸标识符，报的是"未声明的标识符"，而那句话完全不提
 * 版本号，查起来是另一条路。
 */
#ifndef D2O_VERSION
#define D2O_VERSION "0.0.0-unknown"
#endif

namespace d2o {

/// 版本号字面量，形如 "0.1.0"。给 `--version`、状态接口和日志用。
inline constexpr char kVersion[] = D2O_VERSION;

}  // namespace d2o
