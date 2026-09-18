"""Reading-companion proxy.

Sits between the e-reader and whichever AI service you use. The reader speaks
one dialect -- OpenAI-compatible chat completions -- and this translates to
whatever is configured, so changing provider or model is an edit here rather
than a firmware rebuild.

It also means the provider credential lives on this machine and never on the
reader's SD card: the reader carries a token that is only good for this proxy,
so losing the card costs one revocation.

Run:  uvicorn proxy:app --host 127.0.0.1 --port 8099
"""

from __future__ import annotations

import json
import logging
import secrets
import time
import tomllib
from pathlib import Path
from typing import Any, AsyncIterator

import httpx
from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import StreamingResponse

log = logging.getLogger("companion-proxy")

CONFIG_PATH = Path(__file__).with_name("config.toml")


# --------------------------------------------------------------------------- config


def load_config() -> dict[str, Any]:
    if not CONFIG_PATH.exists():
        raise SystemExit(
            f"missing {CONFIG_PATH}\n"
            f"copy config.example.toml to config.toml and fill it in"
        )
    with CONFIG_PATH.open("rb") as handle:
        config = tomllib.load(handle)

    tokens = config.get("auth", {}).get("tokens") or []
    if not tokens or any(not isinstance(t, str) or len(t) < 16 for t in tokens):
        raise SystemExit(
            "config.toml: auth.tokens must hold at least one string of 16+ characters.\n"
            "This is the only thing standing between your API bill and the open internet."
        )
    provider = config.get("provider", {})
    if provider.get("kind") not in ("openai", "anthropic"):
        raise SystemExit('config.toml: provider.kind must be "openai" or "anthropic"')
    if not provider.get("api_key"):
        raise SystemExit("config.toml: provider.api_key is empty")
    if not provider.get("model"):
        raise SystemExit("config.toml: provider.model is empty")
    return config


CONFIG = load_config()
TOKENS: list[str] = CONFIG["auth"]["tokens"]
PROVIDER: dict[str, Any] = CONFIG["provider"]
LIMITS: dict[str, Any] = CONFIG.get("limits", {})

MAX_TOKENS = int(LIMITS.get("max_tokens", 800))
REQUEST_TIMEOUT = float(LIMITS.get("timeout_seconds", 120))
MAX_BODY_BYTES = int(LIMITS.get("max_request_bytes", 64 * 1024))

app = FastAPI(title="Reading companion proxy")


# --------------------------------------------------------------------------- helpers


def authorize(authorization: str | None) -> None:
    """Constant-time check of the reader's bearer token."""
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="missing bearer token")
    presented = authorization[len("Bearer ") :].strip()
    # compare_digest against every token, without short-circuiting on the first
    # match, so timing does not reveal which token was close.
    if not any(secrets.compare_digest(presented, known) for known in TOKENS):
        raise HTTPException(status_code=401, detail="bad token")


def sse(payload: dict[str, Any]) -> bytes:
    return f"data: {json.dumps(payload, ensure_ascii=False)}\n\n".encode("utf-8")


def delta_chunk(text: str, model: str) -> bytes:
    """One OpenAI-shaped content delta -- the only response shape the reader parses."""
    return sse(
        {
            "id": "chatcmpl-companion",
            "object": "chat.completion.chunk",
            "created": int(time.time()),
            "model": model,
            "choices": [{"index": 0, "delta": {"content": text}, "finish_reason": None}],
        }
    )


def finish_chunk(model: str, reason: str = "stop") -> bytes:
    return sse(
        {
            "id": "chatcmpl-companion",
            "object": "chat.completion.chunk",
            "created": int(time.time()),
            "model": model,
            "choices": [{"index": 0, "delta": {}, "finish_reason": reason}],
        }
    )


def error_chunk(message: str) -> bytes:
    """The reader surfaces this text to the user, so say something useful."""
    return sse({"error": {"message": message, "type": "proxy_error"}})


def split_system(messages: list[dict[str, Any]]) -> tuple[str, list[dict[str, Any]]]:
    """Anthropic takes the system prompt as its own field, not as a message."""
    system_parts: list[str] = []
    turns: list[dict[str, Any]] = []
    for message in messages:
        role = message.get("role")
        content = message.get("content") or ""
        if not isinstance(content, str):
            continue
        if role == "system":
            system_parts.append(content)
        elif role in ("user", "assistant"):
            turns.append({"role": role, "content": content})
    return "\n\n".join(system_parts), turns


# --------------------------------------------------------------------------- providers


async def stream_openai(body: dict[str, Any], model: str) -> AsyncIterator[bytes]:
    """Pass through to any OpenAI-compatible endpoint, relaying its SSE verbatim."""
    base = PROVIDER["base_url"].rstrip("/")
    url = f"{base}/chat/completions" if not base.endswith("/chat/completions") else base

    payload = dict(body)
    payload["model"] = model  # the proxy decides the model, not the reader
    payload["stream"] = True
    payload["max_tokens"] = min(int(payload.get("max_tokens") or MAX_TOKENS), MAX_TOKENS)

    headers = {
        "Authorization": f"Bearer {PROVIDER['api_key']}",
        "Content-Type": "application/json",
        "Accept": "text/event-stream",
    }

    async with httpx.AsyncClient(timeout=REQUEST_TIMEOUT) as client:
        async with client.stream("POST", url, json=payload, headers=headers) as response:
            if response.status_code != 200:
                detail = (await response.aread()).decode("utf-8", "replace")[:300]
                log.error("provider %s: %s", response.status_code, detail)
                yield error_chunk(f"上游返回 {response.status_code}")
                yield b"data: [DONE]\n\n"
                return
            # The upstream already speaks the dialect the reader wants; relaying
            # bytes keeps this path free of reassembly bugs.
            async for raw in response.aiter_bytes():
                yield raw


async def stream_anthropic(body: dict[str, Any], model: str) -> AsyncIterator[bytes]:
    """Translate to the Messages API and back into OpenAI-shaped chunks."""
    try:
        import anthropic
    except ImportError:
        yield error_chunk("代理未安装 anthropic 库：pip install anthropic")
        yield b"data: [DONE]\n\n"
        return

    system, turns = split_system(body.get("messages") or [])
    if not turns:
        yield error_chunk("请求里没有对话内容")
        yield b"data: [DONE]\n\n"
        return

    max_tokens = min(int(body.get("max_tokens") or MAX_TOKENS), MAX_TOKENS)
    effort = PROVIDER.get("effort", "low")

    client = anthropic.AsyncAnthropic(
        api_key=PROVIDER["api_key"], timeout=REQUEST_TIMEOUT
    )
    kwargs: dict[str, Any] = {
        "model": model,
        "max_tokens": max_tokens,
        "messages": turns,
        # Replies are a couple of hundred characters and a reader is watching a
        # slow panel, so latency is worth more here than extra deliberation.
        "output_config": {"effort": effort},
    }
    if system:
        kwargs["system"] = system

    try:
        async with client.messages.stream(**kwargs) as stream:
            async for text in stream.text_stream:
                if text:
                    yield delta_chunk(text, model)
            final = await stream.get_final_message()
        if final.stop_reason == "refusal":
            yield error_chunk("模型拒绝了这个请求")
        else:
            yield finish_chunk(model, "length" if final.stop_reason == "max_tokens" else "stop")
    except anthropic.APIStatusError as exc:
        log.error("anthropic %s: %s", exc.status_code, exc.message)
        yield error_chunk(f"上游返回 {exc.status_code}")
    except anthropic.APIConnectionError:
        log.error("anthropic connection failed")
        yield error_chunk("连不上上游服务")
    yield b"data: [DONE]\n\n"


# --------------------------------------------------------------------------- routes


@app.get("/healthz")
async def healthz() -> dict[str, Any]:
    """No auth: it reveals nothing and makes 'is it running' answerable."""
    return {"ok": True, "provider": PROVIDER["kind"], "model": PROVIDER["model"]}


@app.post("/v1/chat/completions")
async def chat_completions(
    request: Request, authorization: str | None = Header(default=None)
) -> StreamingResponse:
    authorize(authorization)

    raw = await request.body()
    if len(raw) > MAX_BODY_BYTES:
        raise HTTPException(status_code=413, detail="request too large")
    try:
        body = json.loads(raw)
    except json.JSONDecodeError:
        raise HTTPException(status_code=400, detail="body is not JSON") from None
    if not isinstance(body.get("messages"), list):
        raise HTTPException(status_code=400, detail="messages must be a list")

    model = PROVIDER["model"]
    log.info(
        "exchange: %d messages, %d bytes -> %s/%s",
        len(body["messages"]),
        len(raw),
        PROVIDER["kind"],
        model,
    )

    generator = (
        stream_anthropic(body, model)
        if PROVIDER["kind"] == "anthropic"
        else stream_openai(body, model)
    )
    return StreamingResponse(
        generator,
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )
