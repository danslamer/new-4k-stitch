# 板端 SSH 免密配置 (rockemb 192.168.137.100)

> 用于开发机 → rocktech 主板免密 SSH. 重烧板子后用这里的公钥恢复.

## 当前配置状态 (2026-07-02)

| 项 | 值 |
|---|---|
| 板子 IP | `192.168.137.100` |
| 板子 hostname | `rockemb` |
| 板子用户 | `rocktech` |
| 板子密码 | `rocktech` (仅首次安装公钥用) |
| 私钥位置 (开发机) | `C:\Users\ASUS\.ssh\rockemb_board_key` (不入仓) |
| 公钥副本 (开发机) | `C:\Users\ASUS\.ssh\rockemb_board_key.pub` |
| 公钥副本 (本目录) | `rockemb_board_key.pub` |
| SSH config 入口 | `~/.ssh/config` 里的 `rk3588-6` / `rockemb` 段 |

## 用法 (开发机)

```bash
# 免密登录
ssh rockemb                              # 用 alias
ssh rocktech@192.168.137.100             # 也行 (走 config 默认 IdentityFile)

# 跑命令
ssh rockemb 'ls -la /dev/dri/'

# 传文件
scp some_file rockemb:~/                # 走 alias
scp -r build/ rockemb:~/Projects/new-4k-stitch/
```

## 重烧板子后恢复 (公钥会丢, 重新装)

```bash
# 方法 1: ssh-copy-id (如果有)
ssh-copy-id -i deploy/ssh/rockemb_board_key.pub rocktech@192.168.137.100
# 提示密码时输入: rocktech

# 方法 2: 没 ssh-copy-id 就手动装
# 开发机上
PUB=$(cat deploy/ssh/rockemb_board_key.pub)
# 一行命令在板子上装 (会提示密码)
ssh rocktech@192.168.137.100 "mkdir -p ~/.ssh && chmod 700 ~/.ssh && \
  echo '$PUB' >> ~/.ssh/authorized_keys && \
  chmod 600 ~/.ssh/authorized_keys"

# 方法 3: 板子串口登录后手动贴
# 板端 (root 或 rocktech):
mkdir -p ~/.ssh && chmod 700 ~/.ssh
# 然后把 rockemb_board_key.pub 的内容贴到 ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys

# 验证
ssh rockemb 'echo OK'                   # 不应再问密码
```

## SSH config 参考 (开发机 `~/.ssh/config` 关键段)

```sshconfig
Host rockemb
    HostName 192.168.137.100
    Port 22
    User rocktech
    IdentityFile C:\Users\ASUS\.ssh\rockemb_board_key
    IdentitiesOnly yes
    PreferredAuthentications publickey
    ServerAliveInterval 30
    ServerAliveCountMax 3
```

`ServerAliveInterval` 防止长 idle 时连接被路由器/NAT 静默断.

## 安全

- **私钥不入仓** — 这只是开发用, 丢了重新生成即可, 不会泄露生产数据
- **公钥可入仓** — 拿到公钥也登录不上 (没有私钥)
- 重烧板子后老公钥失效, 但因为公钥可入仓, 装回很方便
