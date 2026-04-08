# 01. 认证与 SSO

## 1. 账号密码登录

### `POST /api/auth/login`

功能：账号/邮箱 + 密码登录，签发 access/refresh token。

请求示例：

```json
{
  "username": "alice",
  "password": "P@ssw0rd"
}
```

返回示例：

```json
{
  "code": 200,
  "message": "登录成功",
  "data": {
    "token": "<access_token>",
    "refresh_token": "<refresh_token>",
    "expires_in": 10800,
    "refresh_expires_in": 1209600,
    "user": {
      "id": 12,
      "username": "alice",
      "nickname": "Alice",
      "callsign": "BG7XXX",
      "roles": ["user"],
      "approval_status": 1,
      "last_group_id": 999
    }
  }
}
```

## 2. 登出

### `POST /api/auth/logout`

功能：注销 refresh token，并清理 `refresh_token`/`ws_token` Cookie。

返回示例：

```json
{ "code": 200, "message": "登出成功" }
```

## 3. 刷新登录态

### `POST /api/auth/refresh`

功能：刷新 access token（并轮换 refresh token）。

请求示例（非 Cookie 客户端）：

```json
{ "refresh_token": "<refresh_token>" }
```

返回示例：

```json
{
  "code": 200,
  "message": "成功",
  "data": {
    "token": "<new_access_token>",
    "expires_in": 10800,
    "refresh_token": "<new_refresh_token>",
    "refresh_expires_in": 1209600
  }
}
```

## 4. 注册

### `POST /api/auth/register`

功能：邮箱验证码注册（注册后进入待审核状态）。

请求示例：

```json
{
  "username": "alice",
  "password": "P@ssw0rd",
  "callsign": "BG7XXX",
  "email": "alice@example.com",
  "nickname": "Alice",
  "session_id": "email_session_xxx",
  "email_code": "123456"
}
```

返回示例：

```json
{
  "code": 201,
  "message": "注册成功，请等待管理员审核",
  "data": {
    "id": 12,
    "username": "alice",
    "approval_status": 0,
    "device_password": "Ab12Cd34"
  }
}
```

## 5. 邮箱验证码

### `POST /api/auth/send-code`

功能：发送邮箱验证码。用途 `purpose` 支持：
- `register`
- `login`
- `reset_password`
- `change_email`

请求示例：

```json
{
  "email": "alice@example.com",
  "purpose": "register",
  "captcha_id": "cpt_abc",
  "captcha_code": "7k3p"
}
```

返回示例：

```json
{
  "code": 200,
  "message": "验证码已发送",
  "data": {
    "session_id": "email_session_xxx",
    "expires_in": 600
  }
}
```

### `POST /api/auth/verify-email`

功能：注册流程中验证邮箱验证码。

请求：

```json
{ "session_id": "email_session_xxx", "code": "123456" }
```

返回：

```json
{ "code": 200, "message": "邮箱验证成功", "data": { "email": "alice@example.com", "session_id": "email_session_xxx" } }
```

## 6. 邮箱登录与重置密码

### `POST /api/auth/email-login`

请求：

```json
{ "session_id": "email_session_xxx", "code": "123456" }
```

返回：同 `login`，含 `token/refresh_token/user`。

### `POST /api/auth/reset-password`

请求：

```json
{
  "session_id": "email_session_xxx",
  "code": "123456",
  "new_password": "NewPass123"
}
```

返回：

```json
{ "code": 200, "message": "密码重置成功" }
```

## 7. WebSocket Token Cookie

### `POST /api/auth/ws-token/sync`（JWT）

功能：从 `Authorization` 同步 `ws_token` Cookie。

返回：

```json
{ "code": 200, "message": "成功" }
```

### `POST /api/auth/ws-token/clear`

功能：清理 `ws_token`（并清理 `refresh_token`）。

返回：

```json
{ "code": 200, "message": "成功" }
```

## 8. SSO（Keycloak）

### `GET /api/sso/login`
功能：获取 SSO 登录 URL。

返回：

```json
{ "code": 200, "message": "成功", "data": { "url": "https://.../auth?..." } }
```

### `GET /api/sso/callback`
功能：Keycloak 回调入口（服务端处理并重定向前端）。

### `POST /api/sso/exchange`
功能：用一次性交换码换取 token（避免 URL 透传 token）。

请求：

```json
{ "code": "one_time_sso_code" }
```

返回：同 `login`。

### `GET /api/sso/status`（JWT）
返回示例：

```json
{ "code": 200, "message": "成功", "data": { "bound": true, "keycloak_id": "xxxx-xxxx" } }
```

### `POST /api/sso/bind`（JWT）
功能：发起当前用户绑定 SSO。

### `DELETE /api/sso/unbind`（JWT）
功能：解绑当前用户 SSO。
