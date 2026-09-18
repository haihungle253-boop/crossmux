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
| `proxy.py` | 代理本体，单文件，约 265 行 |
| `config.example.toml` | 配置模板 |
| `requirements.txt` | Python 依赖 |
| `companion-proxy.service` | systemd 单元 |
| `README.md` | 完整安装指南（**这份是盲写的，需要你核对**） |


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
cp <仓库路径>/tools/companion-proxy/{proxy.py,config.example.toml,requirements.txt,companion-proxy.service} .

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
```

第 3 步如果报错，`journalctl` 或终端里会有日志，把日志发给 Emrys。


## 任务三：Cloudflare 隧道

Emrys 的域名是 `emrys-huginn.xyz`，托管在 Cloudflare。目标是让
`https://companion.emrys-huginn.xyz` 指向树莓派上的 `127.0.0.1:8099`。

⚠️ **`cloudflared tunnel login` 这一步需要在浏览器里授权，必须 Emrys 本人操作。**
其余步骤你可以做。具体命令见 `README.md` 第二节。

装好之后验证：

```bash
curl https://companion.emrys-huginn.xyz/healthz
```


## 任务四：如果你有余力

这些是加分项，不做也不影响主流程：

1. **限流** —— 目前代理没有限流。令牌一旦泄露，对方可以一直发到额度耗尽。
   建议加在 Cloudflare 那一侧（Rate Limiting 规则），比在代理里写更靠前也更省事。
   如果你更习惯在代理里做，也可以，但请保持 `proxy.py` 的单文件结构。
2. **日志轮转** —— systemd 默认走 journald，一般够用，但如果你发现日志增长很快，
   可以配一下 `journald` 的保留上限。
3. **开机自启验证** —— 装成服务后重启一次树莓派，确认代理和隧道都自动起来了。


## 请反馈给 Emrys 的内容

1. 任务一那五条命令的**实际输出**（这决定我们要不要改 README 和 service 文件）
2. 装的过程中**任何一步和 README 说的不一样**的地方
3. 生成的**令牌**（Emrys 要填进墨水屏）
4. `https://companion.emrys-huginn.xyz/healthz` 能不能通

如果 README 有写错的地方，直接告诉 Emrys，我们会改。这份文档是盲写的，
你在真机上看到的才是事实。
