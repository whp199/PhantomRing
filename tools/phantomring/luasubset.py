"""A very small Lua 5.1 expression subset for fake_mgsv's guest console.

Supported chunks:  `return <expr>[, <expr>...]`  and  `vars.<name> = <expr>`.
Expressions: numbers, strings, nil/true/false, + - * / % ^ .., comparisons (== ~= < <= > >=), and/or/not,
parentheses, `vars.x`, `vars["x"]`, enum tables (BuddyType.WALKER_GEAR...), math.*, tostring, tonumber,
type. Evaluated through a whitelisted Python AST (no eval()).
"""

from __future__ import annotations

import ast
import math
import operator
import re

_TOKEN = re.compile(r"""("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(\.\.)|(~=)|(\^)|(\bnil\b)|(\btrue\b)|(\bfalse\b)|(--.*$)""", re.M)


class LuaError(Exception):
	pass


def tostring(v) -> str:
	if v is None:
		return "nil"
	if v is True:
		return "true"
	if v is False:
		return "false"
	if isinstance(v, (int, float)):
		if isinstance(v, float) and v.is_integer() and abs(v) < 1e15:
			return str(int(v))
		return "%.14g" % v
	if isinstance(v, str):
		return v
	return f"table: 0x{id(v) & 0xFFFFFFFF:08x}"


def tonumber(v):
	if isinstance(v, (int, float)):
		return v
	try:
		f = float(str(v).strip())
		return int(f) if f.is_integer() else f
	except ValueError:
		return None


def luatype(v) -> str:
	if v is None:
		return "nil"
	if isinstance(v, bool):
		return "boolean"
	if isinstance(v, (int, float)):
		return "number"
	if isinstance(v, str):
		return "string"
	if callable(v):
		return "function"
	return "table"


class Table(dict):
	"""Lua-ish table: attribute and index access, missing keys are nil."""

	def __getattr__(self, name):
		return self.get(name)


def _translate(expr: str) -> str:
	def repl(m: re.Match) -> str:
		s, concat, ne, power, nil, true, false, comment = m.groups()
		if s:
			return s
		if concat:
			return " @ "
		if ne:
			return "!="
		if power:
			return "**"
		if nil:
			return "None"
		if true:
			return "True"
		if false:
			return "False"
		return ""

	return _TOKEN.sub(repl, expr)


_BIN = {
	ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv,
	ast.Mod: lambda a, b: a - math.floor(a / b) * b, ast.Pow: operator.pow,
	ast.MatMult: lambda a, b: tostring(a) + tostring(b),
}
_CMP = {ast.Eq: operator.eq, ast.NotEq: operator.ne, ast.Lt: operator.lt, ast.LtE: operator.le, ast.Gt: operator.gt, ast.GtE: operator.ge}


def _eval(node, env):
	match node:
		case ast.Expression(body=b):
			return _eval(b, env)
		case ast.Constant(value=v):
			return v
		case ast.Name(id=name):
			return env.get(name)
		case ast.Attribute(value=v, attr=a):
			obj = _eval(v, env)
			if obj is None:
				raise LuaError(f"attempt to index a nil value (field '{a}')")
			return obj.get(a) if isinstance(obj, dict) else None
		case ast.Subscript(value=v, slice=s):
			obj = _eval(v, env)
			if obj is None:
				raise LuaError("attempt to index a nil value")
			return obj.get(_eval(s, env)) if isinstance(obj, dict) else None
		case ast.BinOp(left=l, op=op, right=r):
			a, b = _eval(l, env), _eval(r, env)
			if type(op) is not ast.MatMult and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
				raise LuaError(f"attempt to perform arithmetic on a {luatype(a if not isinstance(a, (int, float)) else b)} value")
			return _BIN[type(op)](a, b)
		case ast.UnaryOp(op=ast.USub(), operand=o):
			return -_eval(o, env)
		case ast.UnaryOp(op=ast.Not(), operand=o):
			v = _eval(o, env)
			return v is None or v is False
		case ast.BoolOp(op=op, values=vals):
			result = None
			for v in vals:
				result = _eval(v, env)
				truthy = result is not None and result is not False
				if isinstance(op, ast.And) and not truthy:
					return result
				if isinstance(op, ast.Or) and truthy:
					return result
			return result
		case ast.Compare(left=l, ops=ops, comparators=cs):
			a = _eval(l, env)
			for op, c in zip(ops, cs):
				b = _eval(c, env)
				if not _CMP[type(op)](a, b):
					return False
				a = b
			return True
		case ast.Call(func=f, args=args):
			fn = _eval(f, env)
			if not callable(fn):
				raise LuaError("attempt to call a nil value")
			return fn(*[_eval(a, env) for a in args])
		case ast.Tuple(elts=elts):
			return tuple(_eval(e, env) for e in elts)
	raise LuaError(f"unsupported syntax in fake Lua: {type(node).__name__}")


def run_chunk(chunk: str, env: dict) -> str:
	"""Run a chunk; returns the tostring'd return values joined by tabs. Raises LuaError."""
	src = _translate(chunk).strip().rstrip(";").strip()
	if not src:
		return ""
	try:
		if src.startswith("return"):
			body = src[6:].strip()
			if not body:
				return ""
			value = _eval(ast.parse(body, mode="eval"), env)
			values = value if isinstance(value, tuple) else (value,)
			return "\t".join(tostring(v) for v in values)
		m = re.fullmatch(r"vars\s*\.\s*(\w+)\s*=\s*(.+)", src, re.S)
		if m:
			env["vars"][m.group(1)] = _eval(ast.parse(m.group(2), mode="eval"), env)
			return ""
	except SyntaxError as e:
		raise LuaError(f"[string \"chunk\"]:1: syntax error near '{e.text.strip() if e.text else '?'}' (fake Lua subset)") from None
	except (ZeroDivisionError, TypeError, ValueError) as e:
		raise LuaError(str(e)) from None
	raise LuaError("fake_mgsv only runs 'return <expr>' and 'vars.<name> = <expr>'")


def base_env() -> dict:
	return {
		"tostring": tostring, "tonumber": tonumber, "type": luatype,
		"math": Table(pi=math.pi, floor=math.floor, ceil=math.ceil, sqrt=math.sqrt, abs=abs, sin=math.sin, cos=math.cos,
			max=max, min=min, huge=math.inf),
	}
