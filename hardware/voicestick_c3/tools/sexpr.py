"""Minimal KiCad S-expression reader/writer.

Atoms keep their original quoting: quoted strings stay as Python `Q` objects so
that a round trip does not turn names into bare symbols.
"""
import re

_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+')


class Q(str):
    """A quoted string atom."""


def _unquote(token):
    body = token[1:-1]
    return Q(body.replace('\\"', '"').replace('\\\\', '\\'))


def parse(text):
    stack, root = [], None
    for token in _TOKEN.findall(text):
        if token == '(':
            node = []
            if stack:
                stack[-1].append(node)
            elif root is None:
                root = node
            else:
                raise ValueError('multiple roots')
            stack.append(node)
        elif token == ')':
            stack.pop()
        elif token.startswith('"'):
            stack[-1].append(_unquote(token))
        else:
            stack[-1].append(token)
    if stack or root is None:
        raise ValueError('unbalanced expression')
    return root


def quote(value):
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


def atom(value):
    if isinstance(value, Q):
        return quote(value)
    if isinstance(value, bool):
        return 'yes' if value else 'no'
    if isinstance(value, float):
        text = f'{value:.6f}'.rstrip('0').rstrip('.')
        return '0' if text in ('-0', '') else text
    return str(value)


def dump(node, indent=0):
    """Pretty-print with one child list per line, like KiCad does."""
    if not isinstance(node, list):
        return atom(node)
    head = [atom(x) for x in node if not isinstance(x, list)]
    kids = [x for x in node if isinstance(x, list)]
    simple = all(not any(isinstance(y, list) for y in k) for k in kids) and len(kids) <= 3
    if not kids:
        return '(' + ' '.join(head) + ')'
    if simple and sum(len(dump(k)) for k in kids) < 90:
        return '(' + ' '.join(head + [dump(k) for k in kids]) + ')'
    pad = '\t' * (indent + 1)
    out = '(' + ' '.join(head)
    for item in node:
        if isinstance(item, list):
            out += '\n' + pad + dump(item, indent + 1)
    return out + '\n' + '\t' * indent + ')'


def children(node, key):
    return [x for x in node if isinstance(x, list) and x and x[0] == key]


def child(node, key, default=None):
    found = children(node, key)
    return found[0] if found else default


def read_library_symbol(lib_path, name):
    """Return a library symbol with any `extends` chain flattened."""
    lib = parse(open(lib_path, encoding='utf-8').read())
    symbols = {s[1]: s for s in children(lib, 'symbol')}
    sym = symbols[name]
    parent = child(sym, 'extends')
    if parent is None:
        return sym
    base = read_library_symbol(lib_path, parent[1])
    merged = [x for x in base if not (isinstance(x, list) and x[0] in ('property', 'symbol'))]
    merged[1] = Q(name)
    merged += [x for x in sym if isinstance(x, list) and x[0] == 'property']
    for unit in children(base, 'symbol'):
        renamed = list(unit)
        renamed[1] = Q(unit[1].replace(parent[1], name, 1))
        merged.append(renamed)
    return merged


def symbol_pins(sym):
    """Pin number -> (x, y, angle, name, electrical type) in symbol units (mm, y up)."""
    pins = {}
    for unit in children(sym, 'symbol'):
        for pin in children(unit, 'pin'):
            at = child(pin, 'at')
            pins[str(child(pin, 'number')[1])] = (
                float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0,
                str(child(pin, 'name')[1]), pin[1])
    return pins
