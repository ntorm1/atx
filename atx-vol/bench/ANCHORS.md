# Published Performance Anchors

This document anchors all future performance claims to published sources. The table below captures the throughput rates, CPUs, and sources that gate this sprint's benchmarking credibility.

## 1. Anchor Table

| Metric | Number | Hardware | Source |
|---|---:|---|---|
| American prices/s | 45,000 | Ryzen 9 5900, 1 core | https://tastyhedge.com/blog/how-to-calibrate-american-options-really-fast/ |
| American calibrations/s | 16,500 (→ ~1.5M-option US market in ~45 s) | Ryzen 9 5900, 1 core | same |
| European BS IV/s | 2,800,000 | Ryzen 9 5900, 1 core | same (Jaeckel "Let's Be Rational") |
| American prices/s (algo ceiling) | "close to 100,000/s/CPU"; 10–11 digits in <0.1 s | unspecified | SSRN 2547027 (Andersen–Lake–Offengenden) |
| QuantLib QdFp prices/s | ~39k single / ~180k batch (fast); ~12.5k / ~71k (accurate) | unverified CPU | assoc. arXiv:2109.15157; hpcquantlib.wordpress.com |
| AAD Greeks | "up to 1000× vs bumping"; all 1st-order at ~3–4× one price | unspecified | numerix.com AD blog; Giles–Glasserman NA-05-15 |

## 2. Vola Dynamics (the qualitative bar)

Vola Dynamics (https://voladynamics.com/) publishes **no hard numeric performance claim** — only "milliseconds" and "whole US universe on one box in a fraction of a second". Their algorithm and production surface model are not published; public methodology is confined to Klassen's no-arb SSVI/S3 conditions (SSRN 2725700) and cash-dividend pricing (SSRN 2634051). State this explicitly so nobody cites a Vola "number" that does not exist.

## 3. The European-IV ≠ American-IV Caveat

The 2.8M/s Jaeckel figure is **European Black-Scholes IV only**. American IV inversion requires an American pricer in the loop and is ~170× slower at the published anchor (16.5k/s). Any future claim must never compare an American-IV (or American-calibration) rate against the European 2.8M/s figure, and must always name the CPU beside the number.

## 4. Credibility Annotations

- No published American-Greeks throughput number exists anywhere; Greeks targets are a cost model against price/calibration anchors.
- QuantLib QdFp batch numbers (~70–180k/s) could not be pinned to a CPU; treat as order-of-magnitude, not a hardware anchor.
- MatLogica / Numerix AAD speedups are vendor marketing (no CPU, no absolute throughput); use only to justify AAD direction, not as a target multiple.
- No published SIMD/GPU vol calibrator was found; the vectorized-calibrator claim (sprint C2.2) is engineering white space, not a reproduced result.

## 5. House Rules for Citing Anchors

- Every published-anchor comparison cites CPU + source URL.
- Our own numbers are anchor-relative (no third-party engine is built in this repo); say so explicitly when publishing ratios.
- Report unique-contracts/s or calibrations/s alongside positions/s; a headline positions/s without its dedup ratio is rejected.
