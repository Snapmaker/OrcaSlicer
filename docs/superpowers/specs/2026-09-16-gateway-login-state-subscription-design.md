# 登录状态网关订阅设计(移除 SSWCP 登录链路)

**日期:** 2026-09-16
**分支:** feature_flutter_exe
**接口依据:** 飞书《snapmaker_connection 接口文档》(wiki `CWsfwTsWei6MZFkFn5sc0hXMnBy`,revision 607)

## 背景

当前 C++ 侧的登录状态由老 webview/SSWCP 链路闭环维护:登录对话框(OAuth webview)写入
`GUI_App::m_login_userinfo`,`SMUserInfo::set_user_login()` 触发 `user_login_notify()` 向
`m_user_login_subscribers` 推送;web 端通过 `sw_GetUserLoginState` 拉取、`sw_SubscribeUserLoginState`
订阅、`sw_UserLogin`/`sw_AskUserLogin`/`sw_UserLogout` 驱动登录登出。

新架构中登录业务归 Flutter Web + Dart CLI,C++ 不参与登录协议。CLI 已提供:

- `GET /api/account`:冷启动补一帧登录态(平铺 JSON,无 `{ok,data}` 包装);
- `notify.account.changed`:登录/登出/profile 刷新/**CLI 自行发现 token 失效并清会话**时,
  无条件广播给所有 `/ws` 连接(不需要显式 subscribe),`params` 为同一份全量账户快照。

排查结论:除 SnapLog 日志身份(`user_token`/`user_id`,并作为日志上报门禁)和 Preferences
切区时的次要提示外,老链路没有其他真实消费方,其余均为自我闭环。

## 目标

1. C++ 登录态的唯一数据源改为网关账户帧:冷启动/WS 重连 `GET /api/account` 补帧,平时被动收
   `notify.account.changed`。
2. 登录态变化自动同步 SnapLog(`set_user_token`/`set_user_id`),登出(token 置空)即触发
   SnapLog `log()` 入口的未登录丢弃门禁。
3. 删除老登录链路:SSWCP 5 个登录命令、订阅推送、登录对话框、OAuth revoke、静默 token 续期。

## 非目标

- 不实现登录/登出动作接口(C++ 不发 `action.account.login`/`logout`,也不调用 `POST /api/login`/`logout`)。
- 不处理 `notify.region.changed`(换区=登出,先收到的 `account.changed(offline)` 已覆盖登录态)。
- 不处理头像:账户帧无 `icon`/`avatar` 字段,`SMUserInfo` 的 icon 一并移除,web 侧头像由其自持数据渲染。
- 不动 BBS `NetworkAgent`(`m_agent->is_user_login()`)那套登录体系。
- 不改 web(lava-orca)侧代码,但其发版需与本改动对齐(见风险)。

## 账户帧契约

`GET /api/account` 200 响应体与 `notify.account.changed` 的 `params` 为同一份平铺对象:

```json
{
  "is_login": true,  "isLogin": true,
  "status": "online","stauts": "online",
  "userid": "1008611", "id": "1008611", "uid": "1008611",
  "token": "eyJ…", "access_token": "eyJ…", "accessToken": "eyJ…",
  "nickname": "Tian", "account": "…", "email": "…"
}
```

未登录帧中布尔为 `false`、字符串为空、token 族为 `null`。解析规则:

- `is_login`:优先 `is_login`,其次 `isLogin`,最后 `status`/`stauts == "online"` 做兜底;
- `userid`:优先 `userid`,其次 `id`,再次 `uid`;
- `token`:优先 `token`,其次 `access_token`,再次 `accessToken`;`null` → 空串;
- `nickname`/`account`/`email`:字符串直接取,缺失为空;
- 字段类型不符按缺失处理;顶层非对象解析失败。

## 新增 GatewayAccount

新文件 `src/slic3r/Utils/GatewayAccount.hpp/.cpp`(构建注册与 `GatewayDevice` 相同:
`src/slic3r/CMakeLists.txt` 两处源列表 + `tests/slic3rutils/CMakeLists.txt`):

```cpp
namespace Slic3r::Gateway {
struct AccountSnapshot {
    bool        is_login{false};
    std::string userid;
    std::string nickname;
    std::string account;
    std::string email;
    std::string token;
};
class GatewayAccount {
public:
    // frame: /api/account 响应体或 notify.account.changed 的 params;纯解析,无 GUI 依赖。
    static bool parse(const nlohmann::json& frame, AccountSnapshot& out);
};
}
```

## GUI_App 集成

### 状态缓存

- 删除 `SMUserInfo m_login_userinfo`,新增 `Gateway::AccountSnapshot m_gateway_account`。
- 新增 `apply_gateway_account(const AccountSnapshot&)`(UI 线程):
  1. 记录登录态变化日志(online/offline/profile 更新);
  2. 覆盖 `m_gateway_account`;
  3. `SnapLogClient::set_user_token(token)`、`set_user_id(userid)`(Bearer 前缀由 SnapLog 自己加,传裸 token)。

### 通知订阅

在 `register_gateway_notifications()` 注册:

```cpp
m_gateway_service->set_notification_handler("notify.account.changed",
    [this](const nlohmann::json& params) {
        Gateway::AccountSnapshot snapshot;
        if (Gateway::GatewayAccount::parse(params, snapshot))
            apply_gateway_account(snapshot);
        else
            BOOST_LOG_TRIVIAL(warning) << "ignored invalid gateway account frame";
    });
```

回调经 GatewayService dispatcher(实际为 `CallAfter`)在 UI 线程执行,直接改缓存安全。

### 冷启动 / 重连补帧

`set_state_callback` 的 `Connected` 分支触发 `refresh_gateway_account()`:

- 同步 `GET /api/account` 在后台 `std::thread` 执行(最长约 5s,不卡 UI);
- 结果经 `CallAfter` 回 UI 线程,解析成功才 `apply`;
- 用自增 generation 计数丢弃过期补帧(重连竞态下旧响应晚到不得覆盖新状态);
  `notify.account.changed` 全量覆盖不受影响;
- HTTP 失败/未连接:保留当前缓存并记 warning,等待下次重连补帧;
- `Disconnected` 不清缓存(CLI 进程退出≠用户登出,CLI 重启后会恢复会话)。

## 删除清单

### SSWCP 登录命令

从 `m_login_cmd_list` 摘除并删除 handler 实现(同类的 privacy/file/download 等命令保留):
`sw_UserLogin`、`sw_AskUserLogin`、`sw_UserLogout`、`sw_GetUserLoginState`、`sw_SubscribeUserLoginState`。

### 订阅推送链

- `GUI_App::m_user_login_subscribers` 及 `user_login_notify()`;
- `SMUserInfo::notify()`(唯一触发源即 `set_user_login()`);
- `sw_UnsubscribeAll()` / `sw_Webview_Unsubscribe()` / `sw_Unsubscribe_Filter()` /
  `SSWCP::on_webview_delete()` 中针对 login_map 的清理分支。

### 登录 UI 与协议

- `sm_get_login_info()`(`studio_userlogin`/`studio_useroffline` postMessage)、
  `sm_request_login()`、`sm_ShowUserLogin()`、`sm_request_user_logout()`(OAuth revoke);
- `WebSMUserLoginDialog.*`、`SMUserLogin` / `SMAskUserLoginDialog` 相关引用与成员;
- `SMUserInfo` 结构体整体删除。

### 静默 token 续期

`sm_maybe_refresh_login_token()`、`sm_on_token_captured()`、`sm_stop_silent_token_refresh()`、
`sm_is_token_refresh_current()`、`on_silent_refresh_timeout()`、token check timer、
silent refresh timer 及全部 bookkeeping 成员。

### Preferences 切区

删除"已登录则先登出"分支,统一为"切区需要重启"确认;换区触发的登出由 CLI 完成
(C++ 会随后收到 `account.changed(offline)`)。

## 测试计划

新增 `tests/slic3rutils/GatewayAccount_test.cpp`:

1. online 全字段:别名组合(`is_login`+`userid`+`token` 等)解析;
2. 仅次要别名:`isLogin`/`id`/`uid`/`access_token`/`accessToken` 组合;
3. offline 帧:`token:null` → 空串、`is_login:false`;
4. 缺字段/类型不符:安全默认,不抛异常;
5. 顶层非对象:解析失败。

GatewayService 侧补一条 `notify.account.changed` → handler 的分发冒烟(沿用现有 fake WS)。

验证:`gateway_tests` 全量 + `GUI_App.cpp` 单独编译通过。

## 风险与依赖

- **web 对齐(阻塞发布,不阻塞实现):** lava-orca 当前仍通过 WCP `getUserLoginState` 拉登录态,
  本改动删除该命令后,web 必须改为直连 gateway `/ws` 自取(`notify.account.changed` /
  `query.account.current` / `GET /api/account`),否则首页登录显示失效。
- **token 失效自动登出:** 依赖 CLI 广播;若 CLI 缺席该场景,C++ 侧无法自行发现(与老架构一致,
  非回归)。
- **日志上报随登录态启停:** SnapLog 门禁语义不变(未登录不上报),但状态来源更可靠。

## 验收标准

1. 登录/登出/profile 刷新/token 失效四类场景,C++ 缓存与 SnapLog 身份均被动更新;
2. WS 重连后账户状态自动补帧,与 CLI 一致;
3. 仓库内不再存在 `sw_SubscribeUserLoginState`、`m_user_login_subscribers`、`SMUserInfo`、
   静默续期相关符号;
4. `gateway_tests` 与 GUI_App 编译通过。
