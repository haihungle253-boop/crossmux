# 读书搭子 · 树莓派代理

墨水屏和 AI 服务之间的一层。它做三件事：

1. **翻译** —— 墨水屏只会说一种方言（OpenAI 兼容），代理把它转成实际服务商要的格式。
   换服务商、换模型，改这里一行，墨水屏完全不用动，更不用重刷固件。
2. **守住密钥** —— 服务商的真密钥只存在树莓派上。墨水屏拿的是一个**只能访问这个代理**
   的令牌，SD 卡丢了只需注销这一个。
3. **加密** —— 走 Cloudflare 隧道，自动拿到 HTTPS，不用自己申请证书、不用改路由器。


## 一、先把代理跑起来（局域网内）

在树莓派上：

```bash
# 1. 放到一个目录里
mkdir -p ~/companion-proxy && cd ~/companion-proxy
# 把本目录的 proxy.py / config.example.toml / requirements.txt 复制过来
# web/ 这个目录也要一起拷（手机聊天页面在里面），漏了的话服务照样起得来、
# healthz 照样是绿的，只有网页打不开

# 2. 建一个独立的 Python 环境，装依赖
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

# 3. 生成一个令牌，记下来——待会儿墨水屏那边也要填这个
python3 -c "import secrets; print(secrets.token_urlsafe(32))"

# 4. 写配置
cp config.example.toml config.toml
nano config.toml          # 填 tokens / base_url / api_key / model

# 5. 试跑
.venv/bin/uvicorn proxy:app --host 127.0.0.1 --port 8099
```

另开一个终端验证：

```bash
curl http://127.0.0.1:8099/healthz
# 期望：{"ok":true,"provider":"openai","model":"deepseek-chat"}
```

看到这行就说明代理本身没问题了。`Ctrl-C` 停掉，继续下一步。

### 让它开机自启

```bash
sudo cp companion-proxy.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now companion-proxy

systemctl status companion-proxy     # 看状态
journalctl -u companion-proxy -f     # 看实时日志
```

> 服务单元里写的用户名是 `pi`、目录是 `/home/pi/companion-proxy`。
> 如果你的用户名不是 pi，先把文件里这两处改掉。


## 二、Cloudflare 隧道：让墨水屏从外面连得到

### 先说清楚这是在干什么

你有一个域名 `emrys-huginn.xyz`，托管在 Cloudflare。但树莓派在你家路由器后面，
外网默认进不来。

常规做法是「端口转发」：在路由器上开一个洞，把公网流量放进来。这需要你有公网 IP、
要改路由器、要自己搞证书，而且等于把树莓派直接暴露在互联网上。

**Cloudflare 隧道是反过来的**：树莓派主动向 Cloudflare 建立一条长连接，
外面的请求先到 Cloudflare，再顺着这条连接送进来。

好处是：

- **不用改路由器**，不用端口转发，没有公网 IP 也行（很多宽带是 CGNAT，本来就没有）
- **HTTPS 自动就有了**，Cloudflare 那一端负责证书，你不用碰 Let's Encrypt
- 树莓派上**没有任何端口对外开放** —— 代理只监听 `127.0.0.1`

### 动手

```bash
# 1. 装 cloudflared
sudo apt update && sudo apt install -y cloudflared
#    如果仓库里没有，去 Cloudflare 官网下 arm64 的 .deb 包装上

# 2. 授权（会给你一个链接，在浏览器里打开并选择 emrys-huginn.xyz）
cloudflared tunnel login

# 3. 建一条隧道，名字随便起
cloudflared tunnel create companion
#    记下它输出的隧道 ID

# 4. 把域名指向这条隧道
cloudflared tunnel route dns companion companion.emrys-huginn.xyz
```

然后写 `~/.cloudflared/config.yml`：

```yaml
tunnel: companion
credentials-file: /home/pi/.cloudflared/<上一步的隧道ID>.json

ingress:
  - hostname: companion.emrys-huginn.xyz
    service: http://127.0.0.1:8099
  - service: http_status:404
```

启动并设为开机自启：

```bash
cloudflared tunnel run companion          # 先前台跑，确认没报错
sudo cloudflared service install           # 没问题再装成系统服务
```

验证（这次用的是公网地址）：

```bash
curl https://companion.emrys-huginn.xyz/healthz
```

还是那行 `{"ok":true,...}` 就成功了。

> cloudflared 的命令偶尔会随版本变化。如果某一步对不上，
> 以 Cloudflare 官方文档为准：搜索 "Cloudflare Tunnel" → "Create a tunnel"。


## 三、手机聊天页面

代理跑起来之后，用手机浏览器打开同一个地址就是聊天页：

```
https://companion.emrys-huginn.xyz/
```

第一次会问你要令牌（和墨水屏 SD 卡里填的是同一个），存在浏览器里，之后不用再输。

页面顶部显示墨水屏当前读到哪 —— 那是墨水屏每次提问时顺带报告的。
下面是**和墨水屏共用的同一份对话**，墨水屏上聊的那几轮会带一个「墨水屏」标记。

### 为什么这是「一个搭子」而不是「两个」

代理代理了每一次对话，所以它手上那份记录天然是超集：墨水屏的 + 手机的。
墨水屏来提问时，代理**用自己的完整记录替换掉墨水屏发来的那份**。

于是：你下午在手机上争论过某个角色，晚上读完一章按「章末对话」，它记得。

**墨水屏固件一行都不用改。**

如果代理从没见过这本书（刚装好，或者代理当时没开），就原样放行墨水屏自己那份 ——
那时候设备上的记录是唯一的记录。

### 顺带修正「多久没聊了」

墨水屏隔几天没碰这本书，会把第一个选项换成「上次我们聊到哪儿了？」，
并在系统提示里写上「距上次聊过去了几天」。

但它是按**自己本地那份记录**算的，而本地只有它自己聊过的那些。
要是你昨天刚在手机上跟它吵过这本书，墨水屏并不知道，还会一口咬定「过去了 12 天」。

所以代理顺手把这行数字也改对了 —— 和替换历史是同一个道理：超集在代理这边。
它**只修改墨水屏已经写了的那一行**，要不要开口问「上次聊到哪儿」仍然是设备自己决定。

### 历史存在哪

`config.toml` 里的 `history.dir`，一本书一个 JSON 文件，纯文本，可以直接打开看。
`history.turns` 控制保留多少轮，超出的从最老的丢。

> 这个目录含你的对话内容，已经在 `.gitignore` 里了。


## 四、告诉墨水屏

编辑 SD 卡上的 `/companion/config.txt`：

```ini
endpoint = https://companion.emrys-huginn.xyz/v1/chat/completions
model = 随便填，会被代理忽略
key = 第一步生成的那个令牌
```

`model` 那一行现在只是装饰 —— **模型由代理决定**。这正是重点：以后想换模型，
改树莓派上的 `config.toml`，墨水屏不用动。


## 五、安全上要认真对待的几点

| 做了什么 | 为什么 |
|---|---|
| 代理只监听 `127.0.0.1` | 局域网里其他设备也碰不到它，只有隧道能进来 |
| 令牌至少 16 位，启动时校验 | 这是你 API 账单和整个互联网之间**唯一**的一道门 |
| 令牌比较用 `compare_digest` | 避免通过响应时间一点点猜出令牌 |
| 请求体大小上限 | 防止有人用超大请求把内存撑爆 |
| `max_tokens` 上限 | 模型停不下来时，账单也停得下来 |
| systemd 收紧了文件系统权限 | 万一代理被攻破，能碰的东西有限 |

**还没做、你应该知道的**：没有限流。如果令牌泄露了，对方可以一直发请求直到你的额度用完。
真要加的话，Cloudflare 那一侧加一条速率规则比在代理里写更省事，也更靠前。


## 六、出问题时

| 现象 | 多半是 |
|---|---|
| 墨水屏显示「还没设置读书搭子」 | SD 卡上没有 `/companion/config.txt`，或者 endpoint 那行不是 http/https 开头 |
| 墨水屏显示「联系不上搭子」 | 看 `journalctl -u companion-proxy -f`。401 = 两边令牌不一致；其它状态码会写在日志里 |
| `curl .../healthz` 通，但墨水屏连不上 | 墨水屏和外网之间的问题，先确认它连上 Wi-Fi 了 |
| 日志里 `上游返回 401` | 树莓派上的 `api_key` 不对（这是服务商的密钥，不是你自己生成的那个令牌） |
| 日志里 `上游返回 429` | 服务商限流了，等一会儿 |


## 七、这一版还没有的东西

- **限流**：见上。令牌泄露就能一直花你的额度。
- **多人**：所有令牌看到的是同一份历史。这是自用工具，不是多租户服务。
- **手机不能改人设**：墨水屏的人设在 SD 卡上随时可改；手机这边的在
  `config.toml` 的 `[phone]` 里，改完要重启服务。想两边语气一致就把内容抄一份过来。
- **书名靠解析识别**：代理从系统提示里认出「Book: 书名」来区分是哪本书。
  这个格式是我们自己生成的，所以可靠，但让墨水屏直接发一个书籍 ID 会更干净 ——
  那需要改固件，留作以后。
