#pragma once

/**
 * @file src/common/firewall.h
 * @brief Windows 防火墙规则的增删查（走 netsh advfirewall）。
 *
 * 为什么要有这个：**这个程序最可能的失败原因不是代码写错了，而是端口被拦了。**
 * Windows 防火墙对入站连接默认全拦（不只是"公用网络"，专用网络同样拦），
 * 于是本机 `ffplay` 拉流一切正常（回环不走防火墙），而**另一台机器 ——
 * 包括你要接的 NVR —— 连不上**。症状是"NVR 搜不到设备"或"添加时超时"，
 * 和"程序没起来"长得一模一样。
 *
 * 只暴露三件事：查（只读，不需要管理员）、加、删。加和删都要管理员权限，
 * 没有权限时返回一句能直接抄去命令行的话，而不是一个错误码。
 *
 * **不主动改防火墙。** 这个模块只被 `--firewall` 显式调用时才会执行，
 * 启动服务时绝不自动加规则 —— 一个录屏工具静默地把自己的端口对局域网开放，
 * 是使用者完全没有同意过的事。
 */

#include <string>

namespace d2o {

/// 一条入站放行规则。名字固定（不带端口），改端口时是"删了再建"而不是
/// 留一堆旧规则 —— 见 firewall.cpp 里 add 的说明。
struct FirewallRule {
  std::string name;      ///< netsh 里的规则名，也用于按名查询/删除
  std::string protocol;  ///< "TCP" 或 "UDP"
  int port = 0;
  std::string why;       ///< 这条规则是给谁用的，打印给人看
};

/// 规则名的固定前缀，删的时候按它来找我们建的规则。
std::string const& firewall_rule_prefix();

/// 当前进程是否有管理员权限。
///
/// 用 `CreateProcess` 去试 `netsh` 也能间接看出来，但那会真的改一点东西；
/// 这里只读一次令牌信息，没有副作用。
bool is_elevated();

/// 生成"建立这条规则"的 netsh 参数串。
///
/// 单独暴露出来是为了让 `--firewall print` 打出的命令**就是** `--firewall add`
/// 真正会执行的那一条。两处各拼一份的话，迟早会有一处被改而另一处没跟上 ——
/// 而那时"按提示抄命令却没用"是最难查的一种故障（提示本身看着完全合理）。
std::string firewall_rule_add_args(FirewallRule const& r);

/// 查一条规则在不在。**只读，不需要管理员**（netsh 的 show 是查询）。
///
/// `exists` 只在返回 true 时有意义：返回 false 表示"没查成功"（netsh 起不来、
/// 输出看不懂……），和"规则不存在"是两件事，不能都当成 false 了事。
bool firewall_rule_query(std::string const& name, bool& exists, std::string& err);

/// 幂等地建立规则：先按名删（没有就跳过），再加。
///
/// 不是"存在就跳过"：用户改了端口之后，那条旧规则还放行着旧端口、
/// 新端口反而不通，而规则名一模一样，看上去一切正常。
bool firewall_rule_add(FirewallRule const& r, std::string& err);

/// 按名删除规则。规则本来就不存在时返回 **true**（删除是幂等的）。
bool firewall_rule_remove(std::string const& name, std::string& err);

/// 查询是否需要提升权限才能做这件事；不需要时返回空串。
///
/// 单独拎出来是为了让调用方能"先问后做"：没权限时把命令行打出来让人自己跑，
/// 而不是先尝试再加、失败了再解释。
std::string elevation_hint();

}  // namespace d2o
