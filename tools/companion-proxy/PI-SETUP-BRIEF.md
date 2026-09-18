# 给阿吉的任务书 · 树莓派侧验证与部署

> 这份文件是写给在树莓派上工作的助手看的。
> 它是自包含的 —— 不需要了解墨水屏那边的任何上下文。


## 背景（30 秒）

Emrys 有一台 Waveshare 3.97 寸墨水屏阅读器，刷的是 CrossMux 固件。我们在给它加一个
「AI 读书搭子」功能：读完一章可以和 AI 聊这一章。

墨水屏不直接连 AI 服务商，而是连树莓派上的一个小代理。这样做有三个原因：

1. 服务商的真密钥只存在树莓派上，不放在墨水屏的 SD 卡里
2. 换服务商 / 换模型只改树莓派的配置，不用重刷固件
3. 通过 Cloudflare 隧道拿到 HTTPS，不用改路由器

**代理还带一个手机聊天页面。** 墨水屏只有四个按键、没有触摸屏，打字很慢，
所以长篇的自由讨论放在手机上；墨水屏负责「读到哪了」，手机负责「聊」。

两边共用树莓派上的同一份对话记录 —— 这是整件事的关键：代理转发了每一次对话，
所以它手上那份记录是超集。墨水屏提问时，代理会用自己这份记录替换掉墨水屏本地那份，
于是下午在手机上争论过的角色，晚上读完一章在墨水屏上问，它记得。
**这意味着树莓派这一端出问题，两个界面会一起失忆** —— 所以部署要认真对待。

代理已经写好并在云端测试通过了。**现在需要你在真实的树莓派上验证和部署。**


## 代码在哪

```bash
git clone -b claude/eink-ai-reading-companion-a41q62 \
    https://github.com/haihungle253-boop/crossmux.git
cd crossmux/tools/companion-proxy
```

这个目录里：

| 文件 | 说明 |
|---|---|
| `proxy.py` | 代理本体，单文件，约 590 行 |
| `web/index.html` | 手机聊天页面，单文件，无构建步骤、无依赖 |
| `config.example.toml` | 配置模板 |
| `requirements.txt` | Python 依赖 |
| `companion-proxy.service` | systemd 单元 |
| `README.md` | 完整安装指南（**这份是盲写的，需要你核对**） |

运行时还会自己建一个 `history/` 目录（一本书一个 JSON，存对话内容）。
它已经在 `.gitignore` 里 —— **里面是 Emrys 的私人阅读对话，不要提交、不要外发。**


## 任务一：核对环境（最重要）

`README.md` 是在没有树莓派的情况下写的。请先核对这些假设，**把实际结果告诉 Emrys**：

```bash
uname -a                          # 架构和内核
cat /etc/os-release               # 系统版本
python3 --version                 # 需要 >= 3.11（代理用了内置的 tomllib）
whoami                            # systemd 单元里写死了 pi，对不上要改
apt-cache policy cloudflared      # apt 源里有没有 cloudflared
```

**特别需要确认的四点：**

1. **Python 是否 >= 3.11** —— 代理用了 `tomllib`，这是 3.11 才进标准库的。
   如果是 3.9/3.10，告诉我们，我们改成 `tomli` 或换成别的配置格式。
2. **`cloudflared` 能否直接 apt 装到** —— 如果不行，需要从 Cloudflare 官网下
   arm64 的 .deb，请记下实际可用的安装方式。
3. **用户名是不是 `pi`** —— `companion-proxy.service` 里写死了
   `User=pi` 和 `/home/pi/companion-proxy`，不对就要改。
4. **这台树莓派上是否已经有别的服务在用 8099 端口**（`ss -ltnp | grep 8099`）。


## 任务二：装起来并跑通（不联外网也能验）

```bash
mkdir -p ~/companion-proxy && cd ~/companion-proxy
SRC=<仓库路径>/tools/companion-proxy
cp $SRC/{proxy.py,config.example.toml,requirements.txt,companion-proxy.service} .
cp -r $SRC/web .          # ← 手机页面，漏了这一步网页打不开

python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

> `requirements.txt` 里的 `anthropic` 只有在用 Claude 时才需要。
> 如果安装慢或失败，可以先只装 `fastapi uvicorn[standard] httpx`，不影响其它服务商。

生成令牌（**这个值要给 Emrys，他要填进墨水屏 SD 卡**）：

```bash
python3 -c "import secrets; print(secrets.token_urlsafe(32))"
```

写配置：

```bash
cp config.example.toml config.toml
# 编辑 config.toml：
#   auth.tokens     = 上面生成的令牌
#   provider.kind   = "openai"
#   provider.base_url / api_key / model  ← 服务商密钥要问 Emrys 拿，不要自己编
```

另外两节保持默认就行，知道它们干什么即可：

- `[history]` —— `dir` 是对话记录存哪（默认 `history/`，相对于 `proxy.py`），
  `turns` 是每本书保留多少轮（默认 20，超出的从最老的丢）。
- `[phone]` —— 手机页面用的人设。**留空也能跑**，只是搭子没性格。
  这里的人设要改需要重启服务；墨水屏那边的人设在 SD 卡上，改完即时生效。
  人设内容由 Emrys 定，不要替他写。

**注意：`config.toml` 已经在 `.gitignore` 里，不要提交它，里面有密钥。**

启动并自测：

```bash
.venv/bin/uvicorn proxy:app --host 127.0.0.1 --port 8099
```

另开一个终端：

```bash
# 1. 健康检查（不需要令牌）
curl http://127.0.0.1:8099/healthz
# 期望：{"ok":true,"provider":"openai","model":"..."}

# 2. 认证必须拒绝无令牌的请求
curl -s -o /dev/null -w "%{http_code}\n" -X POST http://127.0.0.1:8099/v1/chat/completions \
  -H 'Content-Type: application/json' -d '{"messages":[]}'
# 期望：401

# 3. 带上正确令牌，真的问一句（这一步会花掉服务商的额度）
curl -N -X POST http://127.0.0.1:8099/v1/chat/completions \
  -H "Authorization: Bearer <你生成的令牌>" \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"用一句话介绍你自己"}]}'
# 期望：一串 data: {...} 的流式输出，最后是 data: [DONE]

# 4. 手机页面确实被供出来了
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8099/
# 期望：200。如果是 500，多半是上面 cp -r web 那一步漏了 —— 日志里会写
#   RuntimeError: File at path .../web/index.html does not exist
# 注意这种情况下 healthz 照样是绿的、服务照样起得来，只有网页是坏的。

# 5. 手机侧的两个接口同样必须挡住无令牌的请求
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8099/api/state
curl -s -o /dev/null -w "%{http_code}\n" -X POST http://127.0.0.1:8099/api/chat \
  -H 'Content-Type: application/json' -d '{"message":"hi"}'
# 两条都期望：401
```

第 3 步如果报错，`journalctl` 或终端里会有日志，把日志发给 Emrys。

第 5 步如果返回的不是 401 而是 200，**立刻停下并告诉 Emrys** —— 那说明认证没生效，
一旦接上隧道，任何人都能拿他的账单去聊天。


## 任务三：Cloudflare 隧道

Emrys 的域名是 `emrys-huginn.xyz`，托管在 Cloudflare。目标是让
`https://companion.emrys-huginn.xyz` 指向树莓派上的 `127.0.0.1:8099`。

⚠️ **`cloudflared tunnel login` 这一步需要在浏览器里授权，必须 Emrys 本人操作。**
其余步骤你可以做。具体命令见 `README.md` 第二节。

装好之后验证：

```bash
curl https://companion.emrys-huginn.xyz/healthz
```

隧道通了之后，**这个地址同时就是手机聊天页面**：Emrys 用手机浏览器打开
`https://companion.emrys-huginn.xyz`，第一次会让他粘贴令牌（存在浏览器本地，
只存在他自己手机上）。请你帮他确认页面能打开、深色模式正常、能发出一条消息。

⚠️ 这也意味着**页面本身是公开可达的**，挡在前面的只有那个令牌。
所以令牌要用 `secrets.token_urlsafe(32)` 生成的那种，不要图省事换成短的。


## 任务四：如果你有余力

这些是加分项，不做也不影响主流程：

1. **限流** —— 目前代理没有限流。令牌一旦泄露，对方可以一直发到额度耗尽。
   有了公开可达的手机页面之后这条更值得做了。
   建议加在 Cloudflare 那一侧（Rate Limiting 规则），比在代理里写更靠前也更省事。
   如果你更习惯在代理里做，也可以，但请保持 `proxy.py` 的单文件结构。
2. **备份 `history/`** —— 这是墨水屏和手机共用的那份记录，也是整个功能里
   唯一不可再生的东西：代理、配置都能重装，聊过的话丢了就没了。
   同步到别处时请注意它是 Emrys 的私人阅读对话。
3. **日志轮转** —— systemd 默认走 journald，一般够用，但如果你发现日志增长很快，
   可以配一下 `journald` 的保留上限。
4. **开机自启验证** —— 装成服务后重启一次树莓派，确认代理和隧道都自动起来了。


## 请反馈给 Emrys 的内容

1. 任务一那五条命令的**实际输出**（这决定我们要不要改 README 和 service 文件）
2. 装的过程中**任何一步和 README 说的不一样**的地方
3. 生成的**令牌**（Emrys 要填进墨水屏的 SD 卡，手机页面第一次打开时也要粘这个）
4. `https://companion.emrys-huginn.xyz/healthz` 能不能通
5. 手机浏览器打开 `https://companion.emrys-huginn.xyz` 能不能聊起来
6. 自测第 5 步（`/api/state`、`/api/chat` 无令牌）**是不是真的返回 401**

如果 README 有写错的地方，直接告诉 Emrys，我们会改。这份文档是盲写的，
你在真机上看到的才是事实。
