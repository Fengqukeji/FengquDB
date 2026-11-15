#!/bin/sh
# Everything required is already inside this language SDK folder.
set -u
tool_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
compiler="$tool_dir/Tools/fqdb-codegen"
if [ ! -x "$compiler" ]; then compiler="$tool_dir/Editor/Tools/fqdb-codegen"; fi
if [ ! -x "$compiler" ] && [ -n "${FQDB_CODEGEN:-}" ]; then compiler=$FQDB_CODEGEN; fi
if [ ! -x "$compiler" ]; then
  printf '%s\n' 'Missing bundled generator. Extract the complete language SDK folder.'
  result=1
else
  if [ "$#" -eq 0 ]; then "$compiler" project --sdk "$tool_dir";
  else "$compiler" project --sdk "$tool_dir" --root "$1"; fi
  result=$?
fi
if [ "$result" -eq 0 ]; then printf '%s\n' 'Entity codecs are up to date.'; fi
if [ -t 0 ] && [ -z "${CI:-}" ]; then printf '%s' 'Press Return to close...'; read -r unused || true; fi
exit "$result"
