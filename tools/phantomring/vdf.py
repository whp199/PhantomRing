"""Minimal reader for Valve's text KeyValues (.vdf/.acf) files: quoted or bare tokens, nested braces,
// comments. Keys are kept as written; use get_ci() for Steam's case-insensitive lookups."""

from __future__ import annotations

import re

_TOKEN = re.compile(r'"((?:\\.|[^"\\])*)"|(\{)|(\})|//[^\n]*|([^\s{}"]+)')


def loads(text: str) -> dict:
	stack: list[dict] = [{}]
	key = None
	for m in _TOKEN.finditer(text):
		quoted, opening, closing, bare = m.groups()
		if opening:
			child: dict = {}
			stack[-1][key if key is not None else ""] = child
			stack.append(child)
			key = None
		elif closing:
			if len(stack) > 1:
				stack.pop()
			key = None
		elif quoted is not None or bare is not None:
			token = quoted.replace('\\"', '"').replace("\\\\", "\\") if quoted is not None else bare
			if key is None:
				key = token
			else:
				stack[-1][key] = token
				key = None
	return stack[0]


def load(path) -> dict:
	with open(path, encoding="utf-8", errors="replace") as f:
		return loads(f.read())


def get_ci(d: dict, *keys, default=None):
	"""Nested case-insensitive lookup: get_ci(cfg, 'InstallConfigStore', 'Software', 'Valve')."""
	cur = d
	for k in keys:
		if not isinstance(cur, dict):
			return default
		lk = k.lower()
		for ck, cv in cur.items():
			if ck.lower() == lk:
				cur = cv
				break
		else:
			return default
	return cur
