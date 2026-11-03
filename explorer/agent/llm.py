"""One interface to a language model, whichever one is available.

    complete(system, prompt) -> (text, name of the model) or (None, "none")

The order of preference is the plan's: the Gemini API if a key is set, else a
local model served by Ollama, else no model. With no model the agent still
works from its templates. Only Python's standard library is used.

Settings, all optional, read from the environment or from a .env file in the
project root (which .gitignore keeps out of git):

    GEMINI_API_KEY   turns Gemini on
    GEMINI_API_KEYS  several keys, separated by commas; a rate-limited key is skipped
    GEMINI_MODEL     one model, or several separated by commas, tried in order. The default is
                     gemini-flash-latest, then the faster gemini-flash-lite-latest when the first
                     is overloaded. (Checked on 2026-10-07: older names such as gemini-2.5-flash
                     are refused for new keys.)
    OLLAMA_MODEL     default: qwen2.5-coder:7b
    OLLAMA_URL       default: http://localhost:11434
    PARC_AGENT_LLM   "gemini", "ollama" or "none" to force one choice

A key is only ever sent to Google's API. It is never logged, never cached
and never sent to the browser.
"""

import hashlib
import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CACHE = Path(__file__).resolve().parent.parent / ".cache" / "llm"


def load_env_file(path=ROOT / ".env"):
    """Reads KEY=VALUE lines into the environment, without replacing what is already set."""
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


class ModelError(Exception):
    pass


def _post_json(url, body, headers, timeout):
    request = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                     headers={"Content-Type": "application/json", **headers})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def gemini_keys():
    """The Gemini keys that are set: GEMINI_API_KEYS (comma-separated) and GEMINI_API_KEY."""
    keys = [k.strip() for k in os.environ.get("GEMINI_API_KEYS", "").split(",") if k.strip()]
    single = os.environ.get("GEMINI_API_KEY", "").strip()
    if single and single not in keys:
        keys.insert(0, single)
    return keys


DEFAULT_GEMINI_MODELS = "gemini-flash-latest,gemini-flash-lite-latest"


def gemini_model_names():
    """The Gemini models to try, in order: GEMINI_MODEL, which may list several separated by commas."""
    return [m.strip() for m in os.environ.get("GEMINI_MODEL", DEFAULT_GEMINI_MODELS).split(",") if m.strip()]


class Gemini:
    first_key = 0      # which key to try first; moves on when a key is rate-limited
    last_version = ""  # the exact model that answered last, as Google reports it

    def __init__(self, keys, model=None):
        self.keys = [keys] if isinstance(keys, str) else list(keys)
        self.model = model or gemini_model_names()[0]
        self.name = f"gemini ({self.model})"

    def complete(self, system, prompt):
        url = f"https://generativelanguage.googleapis.com/v1beta/models/{self.model}:generateContent"
        body = {
            "system_instruction": {"parts": [{"text": system}]},
            "contents": [{"role": "user", "parts": [{"text": prompt}]}],
            "generationConfig": {"temperature": 0.2},
        }
        # 429 means this key has used up its quota: the next key is tried.
        # 503 means the model itself is overloaded: one more try, then the
        # caller falls back to the next model, which another key would not fix.
        overloaded = 0
        last = "no key"
        for step in range(len(self.keys)):
            index = (Gemini.first_key + step) % len(self.keys)
            try:
                reply = _post_json(url, body, {"x-goog-api-key": self.keys[index]}, timeout=45)
                parts = reply["candidates"][0]["content"]["parts"]
                Gemini.first_key = index
                Gemini.last_version = reply.get("modelVersion", self.model)
                return "".join(part.get("text", "") for part in parts)
            except urllib.error.HTTPError as problem:
                last = f"HTTP {problem.code}"
                if problem.code == 429:
                    continue
                if problem.code == 503 and overloaded == 0:
                    overloaded += 1
                    time.sleep(2)
                    continue
                raise ModelError(f"Gemini answered HTTP {problem.code}") from problem
            except (urllib.error.URLError, TimeoutError, KeyError, IndexError, ValueError) as problem:
                raise ModelError(f"Gemini could not be used: {type(problem).__name__}") from problem
        raise ModelError(f"every Gemini key is rate-limited ({last})")


class Ollama:
    def __init__(self, model=None, url=None):
        self.model = model or os.environ.get("OLLAMA_MODEL", "qwen2.5-coder:7b")
        self.url = (url or os.environ.get("OLLAMA_URL", "http://localhost:11434")).rstrip("/")
        self.name = f"ollama ({self.model})"

    def complete(self, system, prompt):
        body = {
            "model": self.model,
            "stream": False,
            "options": {"temperature": 0.2},
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": prompt}],
        }
        try:
            return _post_json(f"{self.url}/api/chat", body, {}, timeout=120)["message"]["content"]
        except (urllib.error.URLError, TimeoutError, KeyError, ValueError) as problem:
            raise ModelError(f"Ollama could not be used: {type(problem).__name__}") from problem


_ollama_checked = {"at": 0.0, "up": False}


def _ollama_is_up():
    """Is something listening on the Ollama port? Asked at most once a minute."""
    now = time.monotonic()
    if now - _ollama_checked["at"] > 60 or _ollama_checked["at"] == 0.0:
        url = os.environ.get("OLLAMA_URL", "http://localhost:11434").rstrip("/")
        try:
            with urllib.request.urlopen(f"{url}/api/tags", timeout=1.5):
                _ollama_checked["up"] = True
        except (urllib.error.URLError, TimeoutError, ValueError):
            _ollama_checked["up"] = False
        _ollama_checked["at"] = now
    return _ollama_checked["up"]


def available_models():
    """The models to try, best first."""
    load_env_file()
    forced = os.environ.get("PARC_AGENT_LLM", "").lower()
    if forced == "none":
        return []
    models = []
    keys = gemini_keys()
    if keys and forced in ("", "gemini"):
        models += [Gemini(keys, name) for name in gemini_model_names()]
    if forced == "ollama" or (forced == "" and _ollama_is_up()):
        models.append(Ollama())
    return models


def status():
    """What the explorer shows about the agent: which model answers, or that templates do."""
    models = available_models()
    return {"model": " then ".join(m.name for m in models) if models else "none",
            "note": "" if models else "No language model is set up, so explanations and fixes come from templates. "
                                       "Set GEMINI_API_KEY, or run Ollama, to use a model."}


def _cache_file(model_name, system, prompt):
    digest = hashlib.sha256("\x00".join([model_name, system, prompt]).encode("utf-8")).hexdigest()
    return CACHE / f"{digest}.json"


def complete(system, prompt, models=None, use_cache=True):
    """Asks the first model that works. Returns (text, model name), or (None, "none").

    Answers are cached by a hash of the request, so repeating an evaluation
    does not spend quota again.
    """
    for model in available_models() if models is None else models:
        file = _cache_file(model.name, system, prompt)
        if use_cache and file.is_file():
            try:
                return json.loads(file.read_text(encoding="utf-8"))["text"], model.name
            except (OSError, ValueError, KeyError):
                pass
        try:
            text = model.complete(system, prompt)
        except ModelError:
            continue  # fall back to the next model, and in the end to the templates
        if not text or not text.strip():
            continue
        if use_cache:
            try:
                CACHE.mkdir(parents=True, exist_ok=True)
                file.write_text(json.dumps({"model": model.name, "text": text}), encoding="utf-8")
            except OSError:
                pass
        return text, model.name
    return None, "none"
