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
from fastapi.responses import FileResponse, StreamingResponse

log = logging.getLogger("companion-proxy")
# Without a handler these lines go nowhere, which is exactly when they are
# wanted: journalctl is the first place to look when the reader says it cannot
# reach the companion. Attach to uvicorn's stream so both appear together.
logging.basicConfig(
    level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s"
)

CONFIG_PATH = Path(__file__).with_name("config.toml")
WEB_DIR = Path(__file__).with_name("web")


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

HISTORY_DIR = Path(CONFIG.get("history", {}).get("dir", Path(__file__).with_name("history")))
HISTORY_TURNS = int(CONFIG.get("history", {}).get("turns", 20))
HISTORY_DIR.mkdir(parents=True, exist_ok=True)

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


# --------------------------------------------------------------------------- shared history

# One companion, two surfaces. The proxy sees every exchange from either the
# reader or the phone, so its log is the superset -- which is what lets the
# reader know about a conversation held on the phone without a firmware change.

_BOOK_LINE = "\nBook: "


def book_key(messages: list[dict[str, Any]]) -> str:
    """Which book this exchange belongs to.

    Read out of the system prompt, which the reader builds to a fixed shape
    (PromptBuilder emits "\nBook: <title>"), so this parses our own format
    rather than guessing at someone else's. A dedicated header from the reader
    would be tidier and is the obvious follow-up.
    """
    for message in messages:
        if message.get("role") != "system":
            continue
        text = message.get("content") or ""
        if not isinstance(text, str) or _BOOK_LINE not in text:
            continue
        title = text.split(_BOOK_LINE, 1)[1].split("\n", 1)[0].strip()
        if title:
            # Flattened the same way the reader names its own sidecars, so one
            # book maps to one predictable file.
            safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in title)
            return safe[:80] or "_unknown"
    return "_unknown"


def history_path(book: str) -> Path:
    return HISTORY_DIR / f"{book}.json"


def load_history(book: str) -> list[dict[str, Any]]:
    path = history_path(book)
    if not path.exists():
        return []
    try:
        data = json.loads(path.read_text("utf-8"))
    except (json.JSONDecodeError, OSError):
        log.error("history for %s is unreadable; starting fresh", book)
        return []
    return data if isinstance(data, list) else []


def append_exchange(book: str, question: str, reply: str, source: str) -> None:
    """Recorded only after a reply completes, so a failed turn leaves no half."""
    if not question or not reply:
        return
    entries = load_history(book)
    entries.append(
        {
            "question": question,
            "reply": reply,
            "source": source,  # "reader" or "phone" -- shown in the phone UI
            "at": int(time.time()),
        }
    )
    entries = entries[-HISTORY_TURNS:]
    tmp = history_path(book).with_suffix(".json.tmp")
    try:
        tmp.write_text(json.dumps(entries, ensure_ascii=False, indent=1), "utf-8")
        tmp.replace(history_path(book))
    except OSError as exc:
        log.error("could not save history for %s: %s", book, exc)


_EXCERPT_END = '"""\n\n'


def bare_question(text: str) -> str:
    """The question alone, with the page excerpt the reader wrapped it in removed.

    The reader sends chapter, excerpt and question as one user message, which is
    right for the provider but wrong to store: the phone would display a page of
    the book as something the reader typed, and -- worse -- that stale page text
    would be replayed as a question in every later request.

    This strips our own known wrapper (PromptBuilder's shape), so it is parsing
    a format we emit rather than guessing at one.
    """
    if _EXCERPT_END in text:
        text = text.rsplit(_EXCERPT_END, 1)[1]
    elif text.startswith("[Chapter: "):
        _, _, rest = text.partition("\n")
        text = rest or text
    return text.strip()


def as_turns(entries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    turns: list[dict[str, Any]] = []
    for entry in entries:
        turns.append({"role": "user", "content": entry["question"]})
        turns.append({"role": "assistant", "content": entry["reply"]})
    return turns


def merge_history(body: dict[str, Any], book: str) -> str:
    """Replace the reader's local history with the proxy's fuller record.

    The reader replays what it has, which is only its own turns. When the proxy
    has a log for this book it is the superset -- it recorded those same reader
    turns plus anything said on the phone -- so swapping it in is what makes the
    two surfaces one conversation.

    An empty log means the proxy has not seen this book before (new install, or
    it was down); the reader's own history is then the only record there is and
    is passed through untouched.

    Returns the question being asked, for recording after the reply lands.
    """
    messages = body.get("messages") or []
    system = [m for m in messages if m.get("role") == "system"]
    non_system = [m for m in messages if m.get("role") != "system"]
    question = ""
    for message in reversed(non_system):
        if message.get("role") == "user" and isinstance(message.get("content"), str):
            question = message["content"]
            break

    entries = load_history(book)
    if not entries:
        return bare_question(question)

    body["messages"] = [correct_days_away(m, entries) for m in system] + as_turns(
        entries
    ) + [{"role": "user", "content": question}]
    log.info("replayed %d recorded exchanges for %s", len(entries), book)
    return bare_question(question)


_DAYS_LINE = "Days since you last talked about it: "


def correct_days_away(message: dict[str, Any], entries: list[dict[str, Any]]) -> dict[str, Any]:
    """Fix the reader's "how long has it been" against the proxy's record.

    The e-reader counts from its own local history, which holds only the turns it
    made itself. If the argument about a character happened on the phone
    yesterday, the reader still believes it has been a fortnight -- and says so,
    in the system prompt, as a fact. The proxy has the superset here as
    everywhere else, so it owns this number too.

    Only ever rewrites a line the reader already put there: whether to ask a
    resume question at all stays the device's decision.
    """
    text = message.get("content")
    if not isinstance(text, str) or _DAYS_LINE not in text:
        return message
    last_at = max((int(e.get("at") or 0) for e in entries), default=0)
    if last_at <= 0:
        return message
    days = max(0, int((time.time() - last_at) // 86400))
    lines = [
        f"{_DAYS_LINE}{days}" if line.startswith(_DAYS_LINE) else line
        for line in text.splitlines()
    ]
    corrected = dict(message)
    corrected["content"] = "\n".join(lines)
    return corrected


# --------------------------------------------------------------------------- providers


class ReplyTap:
    """Accumulates the assistant text out of an SSE stream we are relaying.

    The relayed bytes are not touched -- the byte-for-byte path is why the
    OpenAI-compatible backend has no reassembly bugs -- so this parses a copy
    alongside it, purely so the exchange can be recorded afterwards. A parse
    failure here costs a history entry, never the reply the reader is reading.
    """

    def __init__(self) -> None:
        self.text = ""
        self._buffer = ""

    def feed(self, chunk: bytes) -> None:
        self._buffer += chunk.decode("utf-8", "replace")
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            line = line.strip()
            if not line.startswith("data:"):
                continue
            payload = line[len("data:") :].strip()
            if not payload or payload == "[DONE]":
                continue
            try:
                choices = json.loads(payload).get("choices") or []
                delta = (choices[0].get("delta") or {}) if choices else {}
                piece = delta.get("content")
            except (json.JSONDecodeError, AttributeError, IndexError, TypeError):
                continue
            if isinstance(piece, str):
                self.text += piece


async def stream_openai(body: dict[str, Any], model: str, tap: ReplyTap | None = None) -> AsyncIterator[bytes]:
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
            # Most OpenAI-compatible endpoints close with the sentinel, but it
            # is not universal -- and the reader now uses it to tell a finished
            # reply from a dropped connection, so a provider that omits it would
            # have every reply marked incomplete and left out of the history.
            # Watching a small tail catches it even when it lands across a chunk
            # boundary.
            saw_done = False
            tail = b""
            async for raw in response.aiter_bytes():
                if tap is not None:
                    tap.feed(raw)
                if not saw_done:
                    tail = (tail + raw)[-64:]
                    saw_done = b"[DONE]" in tail
                yield raw
            if not saw_done:
                yield b"data: [DONE]\n\n"


async def stream_anthropic(body: dict[str, Any], model: str, tap: ReplyTap | None = None) -> AsyncIterator[bytes]:
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
                    if tap is not None:
                        tap.text += text
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
    book = book_key(body["messages"])
    question = merge_history(body, book)
    remember_position(book, body["messages"])

    log.info(
        "reader asks about %s: %d messages, %d bytes -> %s/%s",
        book,
        len(body["messages"]),
        len(raw),
        PROVIDER["kind"],
        model,
    )
    return StreamingResponse(
        recorded_stream(body, model, book, question, "reader"),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


async def recorded_stream(
    body: dict[str, Any], model: str, book: str, question: str, source: str
) -> AsyncIterator[bytes]:
    """Streams the reply through untouched, then records the completed exchange."""
    tap = ReplyTap()
    generator = (
        stream_anthropic(body, model, tap)
        if PROVIDER["kind"] == "anthropic"
        else stream_openai(body, model, tap)
    )
    try:
        async for chunk in generator:
            yield chunk
    finally:
        # This must be a finally, not a line after the loop. The reader hangs up
        # the moment it sees the provider's [DONE] sentinel rather than waiting
        # for the server to close -- that is deliberate, it saves holding the
        # radio open -- and that hang-up cancels this generator mid-yield. Code
        # placed after the loop never runs, so every reader exchange would go
        # unrecorded and the shared history would only ever contain phone turns.
        #
        # Recording stays after the stream either way: a turn that died part-way
        # leaves whatever text did arrive, which is better than leaving nothing,
        # and append_exchange ignores an empty reply.
        append_exchange(book, question, tap.text.strip(), source)


# --------------------------------------------------------------------------- the phone

# Where the reader currently is, as last reported by a reader request. Held in
# memory only: it is a fact about right now, and a stale position read off disk
# after a restart would be worse than none.
_POSITION: dict[str, dict[str, str]] = {}


def remember_position(book: str, messages: list[dict[str, Any]]) -> None:
    for message in messages:
        if message.get("role") != "system":
            continue
        text = message.get("content") or ""
        if not isinstance(text, str):
            continue
        fields: dict[str, str] = {}
        for line in text.splitlines():
            for label, key in (
                ("Book: ", "book"),
                ("Author: ", "author"),
                ("Current chapter: ", "chapter"),
                ("Progress: ", "progress"),
            ):
                if line.startswith(label):
                    fields[key] = line[len(label) :].strip()
        if fields:
            _POSITION[book] = fields
        return


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(WEB_DIR / "index.html")


@app.get("/api/state")
async def state(
    book: str = "", authorization: str | None = Header(default=None)
) -> dict[str, Any]:
    """What the phone needs to render: where the reader is, and the shared log."""
    authorize(authorization)
    if not book:
        # Default to the most recently touched book, which is almost always the
        # one being read right now.
        books = sorted(HISTORY_DIR.glob("*.json"), key=lambda p: p.stat().st_mtime)
        book = books[-1].stem if books else "_unknown"
    return {
        "book": book,
        "position": _POSITION.get(book, {}),
        "history": load_history(book),
        "provider": PROVIDER["kind"],
        "model": PROVIDER["model"],
    }


@app.post("/api/chat")
async def phone_chat(
    request: Request, authorization: str | None = Header(default=None)
) -> StreamingResponse:
    """A message typed on the phone, answered with the same shared history."""
    authorize(authorization)

    raw = await request.body()
    if len(raw) > MAX_BODY_BYTES:
        raise HTTPException(status_code=413, detail="request too large")
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        raise HTTPException(status_code=400, detail="body is not JSON") from None

    question = (payload.get("message") or "").strip()
    book = (payload.get("book") or "_unknown").strip() or "_unknown"
    if not question:
        raise HTTPException(status_code=400, detail="message is empty")

    persona = CONFIG.get("phone", {}).get("persona", "").strip()
    position = _POSITION.get(book, {})
    lines = [persona] if persona else []
    if position:
        lines.append(
            "\n---\nYou and the reader are reading the same book together and are "
            "at the same point in it."
        )
        for key, label in (
            ("book", "Book"),
            ("author", "Author"),
            ("chapter", "Current chapter"),
            ("progress", "Progress"),
        ):
            if position.get(key):
                lines.append(f"{label}: {position[key]}")
        lines.append(
            "Never reveal, hint at, or speculate about anything beyond this point "
            "in the book, even if asked directly.\nReply in the same language the "
            "reader writes in."
        )

    messages: list[dict[str, Any]] = []
    if lines:
        messages.append({"role": "system", "content": "\n".join(lines)})
    messages.extend(as_turns(load_history(book)))
    messages.append({"role": "user", "content": question})

    body = {"messages": messages, "max_tokens": MAX_TOKENS}
    log.info("phone asks about %s: %d recorded exchanges replayed", book, len(messages) // 2)

    return StreamingResponse(
        recorded_stream(body, PROVIDER["model"], book, question, "phone"),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )
