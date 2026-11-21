// SPDX-License-Identifier: GPL-2.0-or-later
// Browser port of lzss_encode.pack_tree, using Okumura's original match order.
const RING = 4096;
const LOOKAHEAD = 18;
const MASK = RING - 1;
const EMPTY = RING;

export function packTree(data) {
  if (!data.length) return new Uint8Array();

  const ring = new Uint8Array(RING + LOOKAHEAD - 1).fill(0x20);
  const left = new Uint16Array(RING + 1).fill(EMPTY);
  const right = new Uint16Array(RING + 257).fill(EMPTY);
  const parent = new Uint16Array(RING + 1).fill(EMPTY);
  let matchPosition = 0;
  let matchLength = 0;

  function replace(old, node) {
    const previous = parent[old];
    if (right[previous] === old) right[previous] = node;
    else left[previous] = node;
    parent[node] = previous;
    parent[old] = EMPTY;
  }

  function insert(node) {
    let comparison = 1;
    let current = RING + 1 + ring[node];
    right[node] = left[node] = EMPTY;
    matchLength = 0;
    while (true) {
      const branch = comparison >= 0 ? right : left;
      if (branch[current] === EMPTY) {
        branch[current] = node;
        parent[node] = current;
        return;
      }
      current = branch[current];
      let length = 1;
      while (length < LOOKAHEAD) {
        comparison = ring[node + length] - ring[current + length];
        if (comparison) break;
        length++;
      }
      if (length > matchLength) {
        matchPosition = current;
        matchLength = length;
        if (length === LOOKAHEAD) break;
      }
    }
    left[node] = left[current];
    right[node] = right[current];
    parent[left[node]] = parent[right[node]] = node;
    replace(current, node);
  }

  function remove(node) {
    if (parent[node] === EMPTY) return;
    let next;
    if (right[node] === EMPTY) {
      next = left[node];
    } else if (left[node] === EMPTY) {
      next = right[node];
    } else {
      next = left[node];
      if (right[next] !== EMPTY) {
        while (right[next] !== EMPTY) next = right[next];
        right[parent[next]] = left[next];
        parent[left[next]] = parent[next];
        left[next] = left[node];
        parent[left[next]] = next;
      }
      right[next] = right[node];
      parent[right[next]] = next;
    }
    replace(node, next);
  }

  const output = new Uint8Array(data.length + Math.ceil(data.length / 8));
  let flags = 0;
  let write = 1;
  let mask = 1;
  let start = 0;
  let cursor = RING - LOOKAHEAD;
  let remaining = Math.min(LOOKAHEAD, data.length);
  let read = remaining;
  ring.set(data.subarray(0, read), cursor);
  for (let i = 1; i <= LOOKAHEAD; i++) insert(cursor - i);
  insert(cursor);

  while (remaining) {
    matchLength = Math.min(matchLength, remaining);
    if (matchLength < 3) {
      matchLength = 1;
      output[flags] |= mask;
      output[write++] = ring[cursor];
    } else {
      output[write++] = matchPosition & 0xff;
      output[write++] = ((matchPosition >> 4) & 0xf0) | (matchLength - 3);
    }
    mask <<= 1;
    if (mask === 256) {
      flags = write++;
      mask = 1;
    }

    const step = matchLength;
    for (let i = 0; i < step; i++) {
      remove(start);
      if (read < data.length) {
        const byte = data[read++];
        ring[start] = byte;
        if (start < LOOKAHEAD - 1) ring[start + RING] = byte;
      } else {
        remaining--;
      }
      start = (start + 1) & MASK;
      cursor = (cursor + 1) & MASK;
      if (remaining) insert(cursor);
    }
  }

  return output.subarray(0, mask === 1 ? write - 1 : write);
}
