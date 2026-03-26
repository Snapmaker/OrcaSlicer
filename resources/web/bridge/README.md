# Bridge 桥接配置

本目录用于 Flutter/Web 与 C++ 的桥接配置，**Flutter 开发者可通过修改配置文件扩展桥接能力，无需改 C++ 代码**。

## 文件说明

| 文件 | 说明 |
|------|------|
| `bridge_config.json` | 桥接命令配置，定义可配置的 cmd 及其 response |
| `README.md` | 本说明 |

## 如何扩展桥接（Flutter 侧）

1. 在 Flutter 项目的 `web/bridge/` 下放置 `bridge_config.json`
2. 构建时该文件会覆盖 `resources/web/bridge/bridge_config.json`
3. 在 config 的 `handlers` 中添加新 cmd，指定 `response` 或 `read_from` 等

## 配置示例

```json
{
  "handlers": [
    {
      "cmd": "get_theme",
      "read_from": "app_config",
      "keys": ["dark_mode"]
    },
    {
      "cmd": "set_theme",
      "write_to": "app_config",
      "params": ["dark_mode"]
    }
  ]
}
```

## 需要 C++ 原生能力时

若 cmd 需调用 C++ 原生 API（如文件对话框、设备连接），需在 C++ 中 `OrcaBridge::bind("cmd", handler)` 注册，并在 config 的 `native_commands` 中声明。

详见项目根目录 `BRIDGE_CONFIG_DESIGN.md`。
