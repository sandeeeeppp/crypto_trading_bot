"""
precompute.py - Run ONCE before submitting.
Saves model_cache.json with:
  - NAV model weights + best feature cols + best half-life per ETF
  - Risk model weights + best feature cols + best half-life per ETF
  - Unit PnL matrix R for ALL_PROXY (pre-scaled, for fast hedge solving)
  - Top-20 hedge instrument pre-selection (per-portfolio can be computed fast)

Run:  python precompute.py
Output: model_cache.json  (loaded instantly by solution_final.py)
"""

import pandas as pd
import numpy as np
import json
import time
import warnings
warnings.filterwarnings("ignore")

t0 = time.time()

# ------------------------------------------------------------------
# Constants
# ------------------------------------------------------------------

EOD_PRICES  = pd.read_csv("./eod_prices.csv", index_col=0, parse_dates=True).sort_index()

TARGET_ETFS = [f"Target_ETF_{i}" for i in range(1, 6)]
PROXY_ETFS  = [f"Proxy_ETF_{i}" for i in range(1, 11)]
TSY_FUTURES = ["TU", "FV", "TY", "US"]
TSY_BONDS   = ["UST_2Y", "UST_5Y", "UST_10Y", "UST_30Y"]
CDX         = ["CDX_IG_5Y", "CDX_IG_10Y", "CDX_HY_5Y", "CDX_HY_10Y"]

ALL_PROXY   = PROXY_ETFS + TSY_FUTURES + TSY_BONDS + CDX
RISK_PROXY  = TSY_FUTURES + TSY_BONDS + CDX

PAR_QUOTED  = set(TSY_FUTURES + TSY_BONDS + CDX)

COST_BPS = {**{e: 2.0 for e in PROXY_ETFS},
            **{f: 0.5 for f in TSY_FUTURES},
            **{b: 0.5 for b in TSY_BONDS},
            **{c: 1.0 for c in CDX}}

# ------------------------------------------------------------------
# Preprocessing
# ------------------------------------------------------------------

all_prices_filled  = EOD_PRICES.ffill().fillna(0)
all_returns_filled = all_prices_filled.pct_change().dropna()

target_rets     = all_returns_filled[TARGET_ETFS]
proxy_all_rets  = all_returns_filled[[c for c in ALL_PROXY  if c in all_returns_filled.columns]]
proxy_risk_rets = all_returns_filled[[c for c in RISK_PROXY if c in all_returns_filled.columns]]

common_dates = (target_rets.index
                .intersection(proxy_all_rets.index)
                .intersection(proxy_risk_rets.index))

target_rets     = target_rets.loc[common_dates].astype(float)
proxy_all_rets  = proxy_all_rets.loc[common_dates].astype(float)
proxy_risk_rets = proxy_risk_rets.loc[common_dates].astype(float)

# ------------------------------------------------------------------
# Core fitting helpers
# ------------------------------------------------------------------

def _ewma_weights(T, half_life):
    lam = np.exp(-np.log(2.0) / half_life)
    w   = lam ** np.arange(T - 1, -1, -1)
    return w / w.sum()

def _wls(X, y, sample_w):
    sw = np.sqrt(sample_w)
    return np.linalg.lstsq(X * sw[:, None], y * sw, rcond=None)[0]

def _fit(X, y, half_life):
    if half_life is not None:
        return _wls(X, y, _ewma_weights(len(y), half_life))
    w, _, _, _ = np.linalg.lstsq(X, y, rcond=None)
    return w

def _oos_mape(y, prices, X, cols, train_frac=0.60, half_life=None):
    n         = len(y)
    train_end = int(n * train_frac)
    Xk        = X[:, cols]
    step      = max(5, (n - train_end) // 5)
    oos_rets, oos_navs = [], []
    for split in range(train_end, n - 1, step):
        w   = _fit(Xk[:split], y[:split], half_life)
        end = min(split + step, n)
        oos_rets.append(Xk[split:end] @ w)
        oos_navs.append(prices[split + 1: end + 1])
    if not oos_rets:
        return 999.0
    base     = prices[train_end]
    nav_pred = base * np.cumprod(1 + np.concatenate(oos_rets))
    nav_act  = np.concatenate(oos_navs)[:len(nav_pred)]
    return float(np.mean(np.abs(nav_pred - nav_act) / nav_act) * 100)

def _oos_r2(y, X, cols, train_frac=0.60, half_life=None):
    n         = len(y)
    train_end = int(n * train_frac)
    Xk        = X[:, cols]
    step      = max(5, (n - train_end) // 5)
    oos_pred, oos_act = [], []
    for split in range(train_end, n - 1, step):
        w   = _fit(Xk[:split], y[:split], half_life)
        end = min(split + step, n)
        oos_pred.append(Xk[split:end] @ w)
        oos_act.append(y[split:end])
    if not oos_pred:
        return -999.0
    p      = np.concatenate(oos_pred)
    a      = np.concatenate(oos_act)
    ss_res = np.sum((a - p) ** 2)
    ss_tot = np.sum((a - a.mean()) ** 2)
    return float(1.0 - ss_res / (ss_tot + 1e-12))

def _search_mape(y, prices, X, col_names, train_frac=0.60):
    """Joint search over feature count k and EWMA half-life."""
    M          = X.shape[1]
    half_lives = [None, 251, 120, 60]
    corrs      = np.array([abs(np.corrcoef(X[:, j], y)[0, 1]) for j in range(M)])
    order      = np.argsort(corrs)[::-1]
    best_mape  = 999.0
    best_cols  = [order[0]]
    best_hl    = None
    for hl in half_lives:
        for k in range(1, M + 1):
            cols = list(order[:k])
            mape = _oos_mape(y, prices, X, cols, train_frac, hl)
            if mape < best_mape:
                best_mape = mape
                best_cols = cols
                best_hl   = hl
    return best_cols, best_hl, best_mape

def _search_r2(y, X, col_names, train_frac=0.60):
    """Joint search over feature count k and EWMA half-life."""
    M          = X.shape[1]
    half_lives = [None, 251, 120, 60]
    corrs      = np.array([abs(np.corrcoef(X[:, j], y)[0, 1]) for j in range(M)])
    order      = np.argsort(corrs)[::-1]
    best_r2    = -999.0
    best_cols  = list(range(M))
    best_hl    = None
    for hl in half_lives:
        for k in range(1, M + 1):
            cols = list(order[:k])
            r2   = _oos_r2(y, X, cols, train_frac, hl)
            if r2 > best_r2:
                best_r2   = r2
                best_cols = cols
                best_hl   = hl
    return best_cols, best_hl, best_r2

# ------------------------------------------------------------------
# NAV model precompute
# ------------------------------------------------------------------

print("[1/4] Building NAV model (expensive search)...")

proxy_etf_names = [c for c in PROXY_ETFS  if c in proxy_all_rets.columns]
rate_names      = [c for c in (TSY_FUTURES + TSY_BONDS) if c in proxy_all_rets.columns]
credit_names    = [c for c in CDX if c in proxy_all_rets.columns]
rc_names        = rate_names + credit_names

X_pe   = proxy_all_rets[proxy_etf_names].values.astype(float)
X_rc   = proxy_all_rets[rc_names].values.astype(float)

nav_cache = {}  # keyed by ETF name

for etf in TARGET_ETFS:
    print(f"  NAV {etf} ...", end=" ", flush=True)
    t1 = time.time()
    y      = target_rets[etf].values.astype(float)
    prices = all_prices_filled[etf].values

    # Stage 1: proxy ETF subset + half-life
    best_cols, best_hl, best_mape = _search_mape(y, prices, X_pe, proxy_etf_names)

    Xk = X_pe[:, best_cols]
    w1 = _fit(Xk, y, best_hl)

    # Stage 2: rate/credit residual
    resid   = y - Xk @ w1
    r2_rc   = _oos_r2(resid, X_rc, list(range(X_rc.shape[1])), half_life=best_hl)

    rc_weights_kept = {}
    if r2_rc > 0.03:
        w_rc = _fit(X_rc, resid, best_hl)
        col_stds  = X_rc.std(axis=0)
        scaled    = np.abs(w_rc) * col_stds
        threshold = scaled.max() * 0.10
        for i, name in enumerate(rc_names):
            if scaled[i] >= threshold:
                rc_weights_kept[name] = float(w_rc[i])

    nav_cache[etf] = {
        "proxy_cols":  [proxy_etf_names[c] for c in best_cols],  # column names
        "proxy_w":     w1.tolist(),
        "half_life":   best_hl,
        "rc_weights":  rc_weights_kept,   # may be empty
        "oos_mape":    best_mape
    }
    print(f"k={len(best_cols)}, hl={best_hl}, mape={best_mape:.4f}% [{time.time()-t1:.1f}s]")

# ------------------------------------------------------------------
# Risk model precompute
# ------------------------------------------------------------------

print("[2/4] Building Risk model (expensive search)...")

risk_names = [c for c in RISK_PROXY if c in proxy_risk_rets.columns]
X_risk     = proxy_risk_rets[risk_names].values.astype(float)

risk_cache = {}

for etf in TARGET_ETFS:
    print(f"  Risk {etf} ...", end=" ", flush=True)
    t1 = time.time()
    y = target_rets[etf].values.astype(float)

    best_cols, best_hl, best_r2 = _search_r2(y, X_risk, risk_names)

    Xk = X_risk[:, best_cols]
    w  = _fit(Xk, y, best_hl)

    risk_cache[etf] = {
        "risk_cols": [risk_names[c] for c in best_cols],
        "risk_w":    w.tolist(),
        "half_life": best_hl,
        "oos_r2":    best_r2
    }
    print(f"k={len(best_cols)}, hl={best_hl}, r2={best_r2:.4f} [{time.time()-t1:.1f}s]")

# ------------------------------------------------------------------
# Pre-scale unit PnL matrix R for ALL_PROXY (used by hedge solver)
# ------------------------------------------------------------------

print("[3/4] Pre-computing unit PnL matrix for hedge...")

avail_insts = [i for i in ALL_PROXY if i in EOD_PRICES.columns]
prices_full = EOD_PRICES[avail_insts].ffill().fillna(0)
p0          = prices_full.iloc[0]
diffs       = prices_full.diff().iloc[1:]

R_dict = {}  # inst -> list of scaled daily PnL
for inst in avail_insts:
    if inst in PAR_QUOTED:
        scaled = (diffs[inst] / 100.0).tolist()
    else:
        denom  = float(p0[inst])
        scaled = (diffs[inst] / denom if denom != 0.0 else diffs[inst] * 0.0).tolist()
    R_dict[inst] = scaled

R_index = diffs.index.strftime("%Y-%m-%d").tolist()

# Cost vector
cost_arr = {i: COST_BPS[i] / 1e4 if i not in PAR_QUOTED
               else (float(p0[i]) / 100.0) * COST_BPS[i] / 1e4
            for i in avail_insts}

print(f"  R matrix: {len(avail_insts)} instruments x {len(R_index)} days")

# ------------------------------------------------------------------
# Metadata
# ------------------------------------------------------------------

print("[4/4] Saving cache...")

cache = {
    "nav":         nav_cache,
    "risk":        risk_cache,
    "R_dict":      R_dict,
    "R_index":     R_index,
    "avail_insts": avail_insts,
    "cost_arr":    cost_arr,
    "proxy_etf_names": proxy_etf_names,
    "risk_names":      risk_names,
    "par_quoted":      list(PAR_QUOTED),
    "target_etfs":     TARGET_ETFS,
    "data_hash":       str(hash(str(EOD_PRICES.shape) + str(EOD_PRICES.iloc[-1].sum())))
}

with open("model_cache.json", "w") as f:
    json.dump(cache, f, separators=(",", ":"))

total = time.time() - t0
print(f"\nDone. model_cache.json saved. Total time: {total:.1f}s")
print("\nNAV summary:")
for etf, v in nav_cache.items():
    print(f"  {etf}: k={len(v['proxy_cols'])}, hl={v['half_life']}, mape={v['oos_mape']:.4f}%")
print("\nRisk summary:")
for etf, v in risk_cache.items():
    print(f"  {etf}: k={len(v['risk_cols'])}, hl={v['half_life']}, r2={v['oos_r2']:.4f}")
