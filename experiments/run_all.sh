#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

if [[ -x ../.venv/bin/python ]]; then
  PYTHON_BIN=../.venv/bin/python
else
  PYTHON_BIN=python3
fi

echo "============================================================"
echo "  IC-JPEG - benchmark int32"
echo "============================================================"

echo ">>> [1/5] Compilando libimage..."
(cd ../libimage && make shared)

echo ">>> [2/5] Gerando resultados Python..."
"$PYTHON_BIN" compare.py --imgs imgs --out results/comparison

echo ">>> [3/5] Gerando resultados C..."
"$PYTHON_BIN" compare_c.py --imgs imgs --out resultados

echo ">>> [4/5] Gerando plots auxiliares..."
"$PYTHON_BIN" generate_time_plots.py
"$PYTHON_BIN" generate_individual_time_plots.py

echo ">>> [5/5] Verificando snapshot Matrix vs Loeffler..."
"$PYTHON_BIN" check_matrix_loeffler_presentation.py

echo "============================================================"
echo "Concluído. Artefatos em experiments/resultados e experiments/results/comparison"
echo "============================================================"
