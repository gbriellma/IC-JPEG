"""
check_matrix_loeffler_presentation.py - verifica o snapshot versionado de
apresentação Matrix vs Loeffler.
"""

from __future__ import annotations

import csv
from pathlib import Path

HERE = Path(__file__).resolve().parent
RESULT_DIR = HERE / "resultados"
ACTUAL = RESULT_DIR / "matrix_loeffler_presentation.csv"
EXPECTED = RESULT_DIR / "matrix_loeffler_expectation.csv"


def read_csv(path: Path) -> list[dict]:
    with path.open("r", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def main():
    if not ACTUAL.exists():
        raise SystemExit(f"Snapshot atual ausente: {ACTUAL}")
    if not EXPECTED.exists():
        raise SystemExit(f"Snapshot esperado ausente: {EXPECTED}")

    actual = read_csv(ACTUAL)
    expected = read_csv(EXPECTED)
    if actual != expected:
        raise SystemExit(
            "Snapshot Matrix vs Loeffler mudou. "
            "Reveja os resultados e atualize matrix_loeffler_expectation.csv apenas se a mudanca for intencional."
        )

    print("Snapshot Matrix vs Loeffler consistente com a expectativa versionada.")


if __name__ == "__main__":
    main()
