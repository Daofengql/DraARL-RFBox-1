# DraARL Server API 文档（客户端对接版）

> 基于当前代码实现整理（`internal/server/server.go` + `internal/handler/*` + `pkg/websocket/*`）。

## 文档目录

1. [00-约定与鉴权](./00-约定与鉴权.md)
2. [01-认证与SSO](./01-认证与SSO.md)
3. [02-用户与资料](./02-用户与资料.md)
4. [03-设备与配置](./03-设备与配置.md)
5. [04-群组与互联](./04-群组与互联.md)
6. [05-无线电与实时通信](./05-无线电与实时通信.md)
7. [06-通联记录与日志](./06-通联记录与日志.md)
8. [07-资源上传与站点配置](./07-资源上传与站点配置.md)
9. [08-运维与管理接口](./08-运维与管理接口.md)
10. [09-完整路由索引](./09-完整路由索引.md)

## 说明

- 本文档优先使用“推荐新路径”，同时标注兼容旧路径。
- 接口统一返回 `code/message/data`，但少量历史接口（AT 控制）返回业务码 `20000/20001`。
- WebSocket 鉴权使用 `HttpOnly Cookie(ws_token)`，不支持 URL 透传 token。
- 收发控制（`disable_send`/`disable_recv`）仅支持设备级：统一由 `devices` 表与 `PUT /api/devices/:id` 维护。
- 群组维度接口 `PUT /api/groups/:id/devices/:deviceId` 已下线，仅保留 `DELETE /api/groups/:id/devices/:deviceId`（踢出设备）。
