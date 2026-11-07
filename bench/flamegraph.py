#!/usr/bin/env python3
"""Turns `perf script` output into a flame graph SVG and folded stacks.

Usage:
  perf script -F comm,tid,ip,sym,dso -i perf.data | \
      flamegraph.py --title NAME --svg out.svg --folded out.folded

Hover over a frame to see its full name and share of the samples. Frames are
coloured by name, so the same function has the same colour in every graph.
"""

import argparse
import hashlib
import sys
from collections import Counter
from xml.sax.saxutils import escape

WIDTH = 1200
ROW = 16
MARGIN = 10
TOP = 34
MIN_WIDTH_PX = 0.3
CHAR_PX = 6.2


def clean_symbol(symbol, dso):
    if symbol in ("[unknown]", ""):
        return f"[{dso.rsplit('/', 1)[-1]}]"
    symbol = symbol.replace("(anonymous namespace)::", "")
    symbol = symbol.replace("operator()", "operator<call>")
    symbol = symbol.split("(")[0].replace("operator<call>", "operator()")
    return symbol.replace(";", ",").strip()


def read_stacks(lines):
    """Returns a Counter of root-first stacks, keyed by a tuple of frame names."""
    stacks = Counter()
    frames = None
    comm = None
    for line in lines:
        if not line.strip():
            if frames is not None:
                stacks[(comm,) + tuple(reversed(frames))] += 1
            frames = None
        elif line[0] not in " \t":
            comm = line.split()[0]
            frames = []
        elif frames is not None:
            parts = line.strip().split(" ", 1)
            rest = parts[1] if len(parts) > 1 else "[unknown]"
            cut = rest.rfind(" (")
            symbol, dso = (rest[:cut], rest[cut + 2 : -1]) if cut >= 0 else (rest, "unknown")
            frames.append(clean_symbol(symbol, dso))
    if frames is not None:
        stacks[(comm,) + tuple(reversed(frames))] += 1
    return stacks


class Node:
    __slots__ = ("name", "count", "children")

    def __init__(self, name):
        self.name = name
        self.count = 0
        self.children = {}


def build_tree(stacks):
    root = Node("all")
    for stack, count in stacks.items():
        root.count += count
        node = root
        for frame in stack:
            node = node.children.setdefault(frame, Node(frame))
            node.count += count
    return root


def depth_of(node, scale):
    deepest = 0
    for child in node.children.values():
        if child.count * scale >= MIN_WIDTH_PX:
            deepest = max(deepest, depth_of(child, scale))
    return deepest + 1


def colour(name):
    digest = hashlib.md5(name.encode()).digest()
    return f"rgb({205 + digest[0] % 50},{digest[1] % 190 + 40},{digest[2] % 55})"


def render(root, title):
    scale = (WIDTH - 2 * MARGIN) / max(root.count, 1)
    height = TOP + depth_of(root, scale) * ROW + MARGIN
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" '
        f'viewBox="0 0 {WIDTH} {height}" font-family="monospace" font-size="11">',
        f'<rect width="{WIDTH}" height="{height}" fill="#fafafa"/>',
        f'<text x="{MARGIN}" y="20" font-size="14">{escape(title)} '
        f"({root.count:,} samples)</text>",
    ]

    def draw(node, x, depth):
        width = node.count * scale
        y = height - MARGIN - (depth + 1) * ROW
        share = 100.0 * node.count / max(root.count, 1)
        label = escape(node.name)
        out.append(
            f'<g><title>{label} ({node.count:,} samples, {share:.2f}%)</title>'
            f'<rect x="{x:.2f}" y="{y}" width="{width:.2f}" height="{ROW - 1}" '
            f'fill="{colour(node.name)}"/>'
        )
        chars = int((width - 4) / CHAR_PX)
        if chars >= 3:
            text = node.name if len(node.name) <= chars else node.name[: chars - 2] + ".."
            out.append(f'<text x="{x + 2:.2f}" y="{y + 11}">{escape(text)}</text>')
        out.append("</g>")
        child_x = x
        for child in sorted(node.children.values(), key=lambda c: c.name):
            if child.count * scale >= MIN_WIDTH_PX:
                draw(child, child_x, depth + 1)
            child_x += child.count * scale

    draw(root, MARGIN, 0)
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--title", default="flame graph")
    parser.add_argument("--svg", required=True)
    parser.add_argument("--folded", required=True)
    args = parser.parse_args()

    stacks = read_stacks(sys.stdin)
    if not stacks:
        sys.exit("no samples in the input")
    with open(args.folded, "w", encoding="utf-8") as f:
        for stack, count in sorted(stacks.items()):
            f.write(";".join(stack) + f" {count}\n")
    with open(args.svg, "w", encoding="utf-8") as f:
        f.write(render(build_tree(stacks), args.title))


if __name__ == "__main__":
    main()
