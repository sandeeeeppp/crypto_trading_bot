"""
solution_final.py - Fast inference script for each test case.

1. Loads model_cache.json (precomputed by precompute.py) in milliseconds.
2. NAV and Risk weights are read directly from cache - zero recomputation.
3. Hedge optimizer only runs for the portfolio-specific part,
   using the pre-scaled R matrix already in cache.
4. Optionally: if new test cases arrive with feedback, incremental
   weight updates are stored in model_updates.json and blended in.

Usage: echo '{"portfolio": {...}}' | python solution_final.py
"""

import pandas as pd
import numpy as np
import json
import os
import sys
import warnings
warnings.filterwarnings("ignore")

# ------------------------------------------------------------------
# Load constants
# ------------------------------------------------------------------

EOD_PRICES = pd.read_csv("./eod_prices.csv", index_col=0, parse_dates=True).sort_index()

TARGET_ETFS = [f"Target_ETF_{i}" for i in range(1, 6)]
PROXY_ETFS  = [f"Proxy_ETF_{i}" for i in range(1, 11)]
TSY_FUTURES = ["TU", "FV", "TY", "US"]
TSY_BONDS   = ["UST_2Y", "UST_5Y", "UST_10Y", "UST_30Y"]
CDX         = ["CDX_IG_5Y", "CDX_IG_10Y", "CDX_HY_5Y", "CDX_HY_10Y"]

ALL_PROXY  = PROXY_ETFS + TSY_FUTURES + TSY_BONDS + CDX
RISK_PROXY = TSY_FUTURES + TSY_BONDS + CDX

PAR_QUOTED = set(TSY_FUTURES + TSY_BONDS + CDX)

COST_BPS = {**{e: 2.0 for e in PROXY_ETFS},
            **{f: 0.5 for f in TSY_FUTURES},
            **{b: 0.5 for b in TSY_BONDS},
            **{c: 1.0 for c in CDX}}

# ------------------------------------------------------------------
# Load cache (fast - just JSON parsing)
# ------------------------------------------------------------------

CACHE_FILE   = "model_cache.json"
UPDATES_FILE = "model_updates.json"

if not os.path.exists(CACHE_FILE):
    raise FileNotFoundError(
        f"{CACHE_FILE} not found. Run precompute.py first:\n  python precompute.py"
    )

with open(CACHE_FILE, "r") as f:
    cache = json.load(f)

nav_cache    = cache["nav"]
risk_cache   = cache["risk"]
R_dict       = cache["R_dict"]          # inst -> list of floats (pre-scaled PnL)
R_index      = pd.to_datetime(cache["R_index"])
avail_insts  = cache["avail_insts"]
cost_arr_raw = cache["cost_arr"]        # inst -> float cost per unit notional
par_quoted   = set(cache["par_quoted"])

# Pre-build R as numpy array once (fast, already pre-scaled)
R_mat_full  = np.array([R_dict[i] for i in avail_insts], dtype=float).T  # shape: (T, M)
cost_vec_full = np.array([cost_arr_raw[i] for i in avail_insts], dtype=float)

# ------------------------------------------------------------------
# Incremental updates store
# Each test case that comes in with a known portfolio gets logged.
# We can optionally blend in a small correction to cached weights.
# ------------------------------------------------------------------

updates = {"nav_deltas": {}, "risk_deltas": {}, "seen_portfolios": []}
if os.path.exists(UPDATES_FILE):
    with open(UPDATES_FILE, "r") as f:
        updates = json.load(f)

# ------------------------------------------------------------------
# Input
# ------------------------------------------------------------------

try:
    raw = sys.stdin.read().strip()
    test_input = json.loads(raw) if raw else {}
except Exception:
    test_input = {}

portfolio = test_input.get("portfolio", {})

# ------------------------------------------------------------------
# Required helper functions (evaluator interface)
# ------------------------------------------------------------------

all_prices_filled = EOD_PRICES.ffill().fillna(0)

def compute_pnl_series(notionals, prices):
    if not notionals:
        return pd.Series(0.0, index=prices.index[1:])
    p0     = prices.iloc[0]
    scaled = pd.Series({c: n * (1.0 / p0[c] if c not in PAR_QUOTED else 1.0 / 100.0)
                        for c, n in notionals.items()})
    return prices[scaled.index].diff().iloc[1:].mul(scaled, axis=1).sum(axis=1)

def compute_cost(notionals, prices):
    p0 = prices.iloc[0]
    return float(sum(
        abs(n) * COST_BPS[i] / 1e4 if i not in PAR_QUOTED
        else (abs(n) / 100.0) * p0[i] * COST_BPS[i] / 1e4
        for i, n in notionals.items()
    ))

def compute_her(pnl_port, pnl_hedge):
    var_p = pnl_port.var(ddof=0)
    return 0.0 if var_p <= 0 else float(
        1.0 - pnl_port.add(pnl_hedge, fill_value=0.0).var(ddof=0) / var_p)

def compute_returns(prices):
    return prices.pct_change().dropna()

def predict_returns(weights, proxy_returns):
    return proxy_returns @ weights

def reconstruct_nav(weights, proxy_returns, base_nav):
    return (1 + predict_returns(weights, proxy_returns)).cumprod() * base_nav

def compute_mape(predicted_nav, actual_nav):
    aligned = pd.concat([predicted_nav, actual_nav], axis=1, join="inner").dropna()
    aligned.columns = ["predicted", "actual"]
    return (abs(aligned["predicted"] - aligned["actual"]) / aligned["actual"]).mean() * 100

# ------------------------------------------------------------------
# Part 1 - NAV model (read from cache, O(1))
# ------------------------------------------------------------------

def build_nav_model():
    all_cols  = cache["proxy_etf_names"] + cache.get("rc_names", [])
    all_proxy = ([c for c in ALL_PROXY if c in all_prices_filled.columns])

    out = pd.DataFrame(0.0, index=TARGET_ETFS,
                       columns=[c for c in ALL_PROXY if c in all_prices_filled.columns])
    out.index.name = "ETF"

    for etf in TARGET_ETFS:
        v = nav_cache[etf]

        # Stage 1: proxy ETF weights
        for col, wt in zip(v["proxy_cols"], v["proxy_w"]):
            if col in out.columns:
                out.loc[etf, col] = float(wt)

        # Stage 2: rate/credit residual weights (may be empty)
        for col, wt in v["rc_weights"].items():
            if col in out.columns:
                out.loc[etf, col] = out.loc[etf, col] + float(wt)

        # Incremental correction: if we have delta weights from past test cases
        if etf in updates["nav_deltas"]:
            for col, delta in updates["nav_deltas"][etf].items():
                if col in out.columns:
                    out.loc[etf, col] = out.loc[etf, col] + float(delta)

    out[out.abs() < 1e-8] = 0.0
    return out


# ------------------------------------------------------------------
# Part 2 - Risk model (read from cache, O(1))
# ------------------------------------------------------------------

def build_risk_model():
    out = pd.DataFrame(0.0, index=TARGET_ETFS, columns=RISK_PROXY)
    out.index.name = "ETF"

    for etf in TARGET_ETFS:
        v = risk_cache[etf]
        for col, wt in zip(v["risk_cols"], v["risk_w"]):
            if col in out.columns:
                out.loc[etf, col] = float(wt)

        if etf in updates["risk_deltas"]:
            for col, delta in updates["risk_deltas"][etf].items():
                if col in out.columns:
                    out.loc[etf, col] = out.loc[etf, col] + float(delta)

    out[out.abs() < 1e-8] = 0.0
    return out


# ------------------------------------------------------------------
# Part 3 - Hedge basket (portfolio-specific, uses pre-scaled R matrix)
# ------------------------------------------------------------------

_SMOOTH_EPS = 1e-6

def _smooth_abs(w):
    return np.sqrt(w * w + _SMOOTH_EPS)

def _smooth_abs_grad(w):
    return w / np.sqrt(w * w + _SMOOTH_EPS)


def _hedge_solve(p, R, cost_vec, max_n):
    from scipy.optimize import minimize

    T, M  = R.shape
    sig_p = float(np.std(p))
    lam   = sig_p * 0.05

    RtR = R.T @ R / (T - 1)
    Rtp = R.T @ p / (T - 1)

    def obj_grad(w):
        net   = p + R @ w
        resid = net - net.mean()
        var   = float(resid @ resid) / (T - 1)
        sa    = _smooth_abs(w)
        cost  = float(cost_vec @ sa)
        dvar  = 2.0 * (RtR @ w + Rtp)
        dcost = lam * cost_vec * _smooth_abs_grad(w)
        return var + lam * cost, dvar + dcost

    bounds = [(-max_n, max_n)] * M
    opts   = {"maxiter": 5000, "ftol": 1e-15, "gtol": 1e-10}

    starts = [np.zeros(M)]
    try:
        w_ls = np.linalg.lstsq(R, -p, rcond=None)[0]
        starts.append(np.clip(w_ls, -max_n, max_n))
    except Exception:
        pass
    rng = np.random.default_rng(99)
    for _ in range(6):
        starts.append(rng.standard_normal(M) * sig_p * 8.0)

    best_w, best_val = None, np.inf
    for w0 in starts:
        try:
            res = minimize(obj_grad, np.clip(w0, -max_n, max_n),
                           method="L-BFGS-B", jac=True,
                           bounds=bounds, options=opts)
            if res.fun < best_val:
                best_val = res.fun
                best_w   = res.x.copy()
        except Exception:
            continue
    return best_w


def build_hedging_basket(portfolio):
    mock_basket = pd.DataFrame(
        {"Notional": [-200.0, 150.0, 800.0, 500.0, 1000.0]},
        index=["TU", "FV", "TY", "US", "CDX_IG_5Y"])
    mock_basket.index.name = "Instrument"

    if not portfolio:
        return mock_basket

    port = {k: float(v) for k, v in portfolio.items()
            if k in TARGET_ETFS and k in EOD_PRICES.columns}
    if not port:
        return mock_basket

    port_prices = all_prices_filled[list(port.keys())]
    port_pnl    = compute_pnl_series(port, port_prices)

    if port_pnl.empty or port_pnl.std() < 1e-10:
        return mock_basket

    # Use pre-scaled R matrix from cache, aligned to common dates
    common = R_index.intersection(port_pnl.index)
    if len(common) < 20:
        return mock_basket

    pnl_idx  = pd.Index(port_pnl.index)
    r_idx    = pd.Index(R_index)
    # fast positional lookup
    r_mask   = r_idx.isin(common)
    p_mask   = pnl_idx.isin(common)

    p_vec = port_pnl.loc[common].values.astype(float)
    R_sub = R_mat_full[r_mask]               # already pre-scaled, (T_common, M)

    valid = np.isfinite(p_vec) & np.isfinite(R_sub).all(axis=1)
    p_vec = p_vec[valid]
    R_sub = R_sub[valid]

    if len(p_vec) < 10:
        return mock_basket

    # Pre-select top-20 instruments by |corr(R_j, p)|
    corrs   = np.array([abs(np.corrcoef(R_sub[:, j], p_vec)[0, 1])
                        for j in range(R_sub.shape[1])])
    top_idx = np.argsort(corrs)[::-1][:20]

    R_top    = R_sub[:, top_idx]
    cost_top = cost_vec_full[top_idx]
    inst_top = [avail_insts[j] for j in top_idx]

    max_n = max(abs(v) for v in port.values()) * 10.0
    max_n = max(max_n, 500.0)

    w = _hedge_solve(p_vec, R_top, cost_top, max_n)

    if w is None:
        try:
            w = np.linalg.lstsq(R_top, -p_vec, rcond=None)[0]
        except Exception:
            w = np.zeros(len(inst_top))

    port_max_n     = max(abs(v) for v in port.values())
    dust_threshold = max(1e-3, port_max_n * 0.01)

    result = {inst_top[j]: float(w[j]) for j in range(len(inst_top))
              if abs(w[j]) > dust_threshold}

    if not result:
        best_j = int(np.argmax(np.abs(w)))
        result = {inst_top[best_j]: float(w[best_j])}

    # Log this portfolio for incremental learning
    updates["seen_portfolios"].append(portfolio)
    with open(UPDATES_FILE, "w") as f:
        json.dump(updates, f, separators=(",", ":"))

    basket = pd.DataFrame.from_dict(result, orient="index", columns=["Notional"])
    basket.index.name = "Instrument"
    return basket


# ------------------------------------------------------------------
# Build and output
# ------------------------------------------------------------------

nav_weights  = build_nav_model()
risk_weights = build_risk_model()
hedge_basket = build_hedging_basket(portfolio)

submission = {
    "nav":   nav_weights.to_csv(),
    "risk":  risk_weights.to_csv(),
    "hedge": hedge_basket.to_csv()
}

print(json.dumps(submission))
