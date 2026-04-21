#!/usr/bin/env python3
"""
Complex primitive code generator.

Reads YAML schema, generates:
- C header: struct definition, serialization API, offset constants
- C source: serialization implementation
- WGSL: accessor functions with calculated offsets

Usage: python generate.py <schema.yaml>
Output files written to same directory as schema.
"""

import sys
import yaml
from pathlib import Path

# Type info: (c_type, wgsl_type, size_words, wgsl_cast)
TYPES = {
    'f32': ('float', 'f32', 1, 'bitcast<f32>'),
    'u32': ('uint32_t', 'u32', 1, ''),
    'i32': ('int32_t', 'i32', 1, 'bitcast<i32>'),
}


def load_schema(path):
    with open(path) as f:
        return yaml.safe_load(f)


def calculate_layout(schema):
    """Calculate word offsets for uniforms and buffers."""
    offset = 0
    uniforms = []
    buffers = []

    # Uniforms first
    for u in schema.get('uniforms', []):
        count = u.get('count', 1)
        t = TYPES[u['type']]
        uniforms.append({
            'name': u['name'],
            'type': u['type'],
            'c_type': t[0],
            'wgsl_type': t[1],
            'wgsl_cast': t[2],
            'count': count,
            'offset': offset,
        })
        offset += t[2] * count

    uniforms_size = offset

    # Buffer length fields
    for b in schema.get('buffers', []):
        buffers.append({
            'name': b['name'],
            'element_type': b['element_type'],
            'len_offset': offset,
        })
        offset += 1  # length word

    header_size = offset  # total fixed header size

    # Buffer data offsets (calculated at runtime, after all lengths)
    data_offset = offset
    for b in buffers:
        b['data_offset'] = data_offset
        # Actual offset depends on previous buffer lengths - runtime calc

    return uniforms, buffers, uniforms_size, header_size


def generate_c_header(schema, uniforms, buffers, uniforms_size, header_size):
    name = schema['name']
    NAME = name.upper()
    type_id = schema['type_id']
    if isinstance(type_id, str):
        type_id = int(type_id, 16)

    lines = [
        f'// Auto-generated from {name}.yaml - DO NOT EDIT',
        f'#pragma once',
        f'',
        f'#include <stdint.h>',
        f'#include <stddef.h>',
        f'#include <yetty/ycore/result.h>',
        f'',
        f'#ifdef __cplusplus',
        f'extern "C" {{',
        f'#endif',
        f'',
        f'#define YETTY_{NAME}_TYPE_ID 0x{type_id:08x}u',
        f'#define YETTY_{NAME}_UNIFORMS_SIZE {uniforms_size}',
        f'#define YETTY_{NAME}_HEADER_SIZE {header_size}',
        f'',
        f'// Uniform offsets (words from payload start)',
    ]

    for u in uniforms:
        lines.append(f'#define YETTY_{NAME}_{u["name"].upper()}_OFFSET {u["offset"]}')

    lines.append('')
    lines.append('// Buffer length offsets')
    for b in buffers:
        lines.append(f'#define YETTY_{NAME}_{b["name"].upper()}_LEN_OFFSET {b["len_offset"]}')

    lines.extend([
        f'',
        f'// Uniforms struct',
        f'struct yetty_{name}_uniforms {{',
    ])

    for u in uniforms:
        if u['count'] > 1:
            lines.append(f'    {u["c_type"]} {u["name"]}[{u["count"]}];')
        else:
            lines.append(f'    {u["c_type"]} {u["name"]};')

    lines.extend([
        f'}};',
        f'',
    ])

    # Serialization API - one buffer param per buffer in schema
    buf_params = ', '.join([f'const uint32_t *{b["name"]}, size_t {b["name"]}_len' for b in buffers])

    lines.extend([
        f'// Serialization',
        f'size_t yetty_{name}_serialized_size(',
        f'    const struct yetty_{name}_uniforms *uniforms,',
        f'    {buf_params});',
        f'',
        f'struct yetty_core_size_result yetty_{name}_serialize(',
        f'    const struct yetty_{name}_uniforms *uniforms,',
        f'    {buf_params},',
        f'    uint8_t *out, size_t out_capacity);',
        f'',
        f'#ifdef __cplusplus',
        f'}}',
        f'#endif',
    ])

    return '\n'.join(lines)


def generate_c_source(schema, uniforms, buffers, uniforms_size, header_size):
    name = schema['name']
    NAME = name.upper()

    buf_params = ', '.join([f'const uint32_t *{b["name"]}, size_t {b["name"]}_len' for b in buffers])
    buf_len_sum = ' + '.join([f'{b["name"]}_len' for b in buffers]) or '0'

    lines = [
        f'// Auto-generated from {name}.yaml - DO NOT EDIT',
        f'',
        f'#include "{name}-gen.h"',
        f'#include <string.h>',
        f'',
        f'size_t yetty_{name}_serialized_size(',
        f'    const struct yetty_{name}_uniforms *uniforms,',
        f'    {buf_params})',
        f'{{',
        f'    (void)uniforms;',
        f'    // FAM header (2) + uniforms + buffer lengths + buffer data',
        f'    return (2 + YETTY_{NAME}_HEADER_SIZE + {buf_len_sum}) * sizeof(uint32_t);',
        f'}}',
        f'',
        f'struct yetty_core_size_result yetty_{name}_serialize(',
        f'    const struct yetty_{name}_uniforms *uniforms,',
        f'    {buf_params},',
        f'    uint8_t *out, size_t out_capacity)',
        f'{{',
        f'    if (!uniforms)',
        f'        return YETTY_ERR(yetty_core_size, "uniforms is NULL");',
        f'    if (!out)',
        f'        return YETTY_ERR(yetty_core_size, "out is NULL");',
        f'',
        f'    size_t total_buf_words = {buf_len_sum};',
        f'    size_t required = (2 + YETTY_{NAME}_HEADER_SIZE + total_buf_words) * sizeof(uint32_t);',
        f'    if (out_capacity < required)',
        f'        return YETTY_ERR(yetty_core_size, "buffer too small");',
        f'',
        f'    uint32_t *p = (uint32_t *)out;',
        f'',
        f'    // FAM header',
        f'    *p++ = YETTY_{NAME}_TYPE_ID;',
        f'    *p++ = (uint32_t)(required - 2 * sizeof(uint32_t));',
        f'',
        f'    // Uniforms',
        f'    memcpy(p, uniforms, sizeof(struct yetty_{name}_uniforms));',
        f'    p += YETTY_{NAME}_UNIFORMS_SIZE;',
        f'',
    ]

    # Write buffer lengths
    for b in buffers:
        lines.append(f'    *p++ = (uint32_t){b["name"]}_len;')

    lines.append('')

    # Write buffer data
    for b in buffers:
        lines.extend([
            f'    if ({b["name"]} && {b["name"]}_len > 0)',
            f'        memcpy(p, {b["name"]}, {b["name"]}_len * sizeof(uint32_t));',
            f'    p += {b["name"]}_len;',
            f'',
        ])

    lines.extend([
        f'    return YETTY_OK(yetty_core_size, required);',
        f'}}',
    ])

    return '\n'.join(lines)


def main():
    if len(sys.argv) != 2:
        print(f'Usage: {sys.argv[0]} <schema.yaml>', file=sys.stderr)
        sys.exit(1)

    schema_path = Path(sys.argv[1])
    schema = load_schema(schema_path)
    uniforms, buffers, uniforms_size, header_size = calculate_layout(schema)

    name = schema['name']
    out_dir = schema_path.parent

    header = generate_c_header(schema, uniforms, buffers, uniforms_size, header_size)
    source = generate_c_source(schema, uniforms, buffers, uniforms_size, header_size)

    (out_dir / f'{name}-gen.h').write_text(header + '\n')
    (out_dir / f'{name}-gen.c').write_text(source + '\n')

    print(f'Generated:')
    print(f'  {out_dir / f"{name}-gen.h"}')
    print(f'  {out_dir / f"{name}-gen.c"}')


if __name__ == '__main__':
    main()
