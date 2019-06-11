#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright 2019 The MLIR Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Script for updating SPIR-V dialect by scraping information from SPIR-V
# HTML and JSON specs from the Internet.
#
# For example, to define the enum attribute for SPIR-V memory model:
#
# ./gen_spirv_dialect.py --bash_td_path /path/to/SPIRVBase.td \
#                        --new-enum MemoryModel
#
# The 'operand_kinds' dict of spirv.core.grammar.json contains all supported
# SPIR-V enum classes.

import requests

SPIRV_HTML_SPEC_URL = 'https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html'
SPIRV_JSON_SPEC_URL = 'https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/master/include/spirv/unified1/spirv.core.grammar.json'

AUTOGEN_ENUM_SECTION_MARKER = 'enum section. Generated from SPIR-V spec; DO NOT MODIFY!'


def get_spirv_grammar_from_json_spec():
  """Extracts operand kind and instruction grammar from SPIR-V JSON spec.

    Returns:
        - A list containing all operand kinds' grammar
        - A list containing all instructions' grammar
    """
  response = requests.get(SPIRV_JSON_SPEC_URL)
  spec = response.content

  import json
  spirv = json.loads(spec)

  return spirv['operand_kinds'], spirv['instructions']


def split_list_into_sublists(items, offset):
  """Split the list of items into multiple sublists.

    This is to make sure the string composed from each sublist won't exceed
    80 characters.

    Arguments:
        - items: a list of strings
        - offset: the offset in calculating each sublist's length
    """
  chuncks = []
  chunk = []
  chunk_len = 0

  for item in items:
    chunk_len += len(item) + 2
    if chunk_len > 80:
      chuncks.append(chunk)
      chunk = []
      chunk_len = len(item) + 2
    chunk.append(item)

  if len(chunk) != 0:
    chuncks.append(chunk)

  return chuncks


def gen_operand_kind_enum_attr(operand_kind):
  """Generates the TableGen EnumAttr definition for the given operand kind.

    Returns:
        - The operand kind's name
        - A string containing the TableGen EnumAttr definition
    """
  if 'enumerants' not in operand_kind:
    return '', ''

  kind_name = operand_kind['kind']
  kind_acronym = ''.join([c for c in kind_name if c >= 'A' and c <= 'Z'])
  kind_cases = [(case['enumerant'], case['value'])
                for case in operand_kind['enumerants']]
  max_len = max([len(symbol) for (symbol, _) in kind_cases])

  # Generate the definition for each enum case
  fmt_str = 'def SPV_{acronym}_{symbol} {colon:>{offset}} '\
            'EnumAttrCase<"{symbol}">;'
  case_defs = [fmt_str.format(acronym=kind_acronym, symbol=case[0],
                              colon=':', offset=(max_len + 1 - len(case[0])))
               for case in kind_cases]
  case_defs = '\n'.join(case_defs)

  # Generate the list of enum case names
  fmt_str = 'SPV_{acronym}_{symbol}';
  case_names = [fmt_str.format(acronym=kind_acronym,symbol=case[0])
                for case in kind_cases]

  # Split them into sublists and concatenate into multiple lines
  case_names = split_list_into_sublists(case_names, 6)
  case_names = ['{:6}'.format('') + ', '.join(sublist)
                for sublist in case_names]
  case_names = ',\n'.join(case_names)

  # Generate the enum attribute definition
  enum_attr = 'def SPV_{name}Attr :\n    '\
      'EnumAttr<"{name}", "valid SPIR-V {name}", [\n{cases}\n    ]>;'.format(
          name=kind_name, cases=case_names)
  return kind_name, case_defs + '\n' + enum_attr


def update_td_enum_attrs(path, operand_kinds, filter_list):
  """Updates SPIRBase.td with new generated enum definitions.

    Arguments:
        - path: the path to SPIRBase.td
        - operand_kinds: a list containing all operand kinds' grammar
        - filter_list: a list containing new enums to add
    """
  with open(path, 'r') as f:
    content = f.read()

  content = content.split(AUTOGEN_ENUM_SECTION_MARKER)
  assert len(content) == 3

  # Extend filter list with existing enum definitions
  import re
  existing_kinds = [
      k[8:-4] for k in re.findall('def SPV_\w+Attr', content[1])]
  filter_list.extend(existing_kinds)

  # Generate definitions for all enums in filter list
  defs = [gen_operand_kind_enum_attr(kind)
          for kind in operand_kinds if kind['kind'] in filter_list]
  # Sort alphabetically according to enum name
  defs.sort(key=lambda enum : enum[0])
  # Only keep the definitions from now on
  defs = [enum[1] for enum in defs]

  # Substitute the old section
  content = content[0] + AUTOGEN_ENUM_SECTION_MARKER + '\n\n' + \
      '\n\n'.join(defs) + "\n\n// End " + AUTOGEN_ENUM_SECTION_MARKER  \
      + content[2];

  with open(path, 'w') as f:
    f.write(content)


if __name__ == '__main__':
  import argparse

  cli_parser = argparse.ArgumentParser(
      description='Update SPIR-V dialect definitions using SPIR-V spec')
  cli_parser.add_argument('--base-td-path', dest='base_td_path', type=str,
                          help='Path to SPIRVBase.td')
  cli_parser.add_argument('--new-enum', dest='new_enum', type=str,
                          help='SPIR-V enum to be added to SPIRVBase.td')
  args = cli_parser.parse_args()

  operand_kinds, _ = get_spirv_grammar_from_json_spec()

  update_td_enum_attrs(args.base_td_path, operand_kinds, [args.new_enum])
