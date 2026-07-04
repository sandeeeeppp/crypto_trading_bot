import pandas as pd
import numpy as np
import json
import os
import sys
import warnings
warnings.filterwarnings('ignore')

##### DO NOT MODIFY/DELETE THE BELOW CODE #############################################################################

EOD_PRICES   = pd.read_csv("./eod_prices.csv", index_col=0, parse_dates=True).sort_index()

TARGET_ETFS  = [f"Target_ETF_{i}" for i in range(1, 6)]
PROXY_ETFS   = [f"Proxy_ETF_{i}" for i in range(1, 11)]
TSY_FUTURES  = ["TU", "FV", "TY", "US"]
TSY_BONDS    = ["UST_2Y", "UST_5Y", "UST_10Y", "UST_30Y"]
CDX          = ["CDX_IG_5Y", "CDX_IG_10Y", "CDX_HY_5Y", "CDX_HY_10Y"]

ALL_PROXY    = PROXY_ETFS + TSY_FUTURES + TSY_BONDS + CDX
RISK_PROXY   = TSY_FUTURES + TSY_BONDS + CDX
PAR_QUOTED   = set(TSY_FUTURES + TSY_BONDS + CDX)

COST_BPS = {**{e: 2.0 for e in PROXY_ETFS},
            **{f: 0.5 for f in TSY_FUTURES},
            **{b: 0.5 for b in TSY_BONDS},
            **{c: 1.0 for c in CDX}}

def compute_returns(prices):
    return prices.pct_change().dropna()

def predict_returns(weights, proxy_returns):
    return proxy_returns @ weights

def reconstruct_nav(weights, proxy_returns, base_nav):
    pred_returns = predict_returns(weights, proxy_returns)
    return (1 + pred_returns).cumprod() * base_nav

def compute_mape(predicted_nav, actual_nav):
    aligned = pd.concat([predicted_nav, actual_nav], axis=1, join="inner").dropna()
    aligned.columns = ["predicted", "actual"]
    return (abs(aligned["predicted"] - aligned["actual"]) / aligned["actual"]).mean() * 100

def compute_pnl_series(notionals, prices):
    if not notionals:
        return pd.Series(0.0, index=prices.index[1:])
    p0 = prices.iloc[0]
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

#######################################################################################################################
# YOUR CODE STARTS HERE
#######################################################################################################################

# ------------------------------------------------------------------
# Preprocessing (done once, shared by all build functions)
# ------------------------------------------------------------------

_prices_filled  = EOD_PRICES.ffill().fillna(0)
_rets_filled    = _prices_filled.pct_change().dropna()

_target_rets     = _rets_filled[TARGET_ETFS]
_proxy_all_rets  = _rets_filled[[c for c in ALL_PROXY  if c in _rets_filled.columns]]
_proxy_risk_rets = _rets_filled[[c for c in RISK_PROXY if c in _rets_filled.columns]]

_common = (_target_rets.index
           .intersection(_proxy_all_rets.index)
           .intersection(_proxy_risk_rets.index))

_target_rets     = _target_rets.loc[_common].astype(float)
_proxy_all_rets  = _proxy_all_rets.loc[_common].astype(float)
_proxy_risk_rets = _proxy_risk_rets.loc[_common].astype(float)

_proxy_etf_names = [c for c in PROXY_ETFS  if c in _proxy_all_rets.columns]
_rate_names      = [c for c in (TSY_FUTURES + TSY_BONDS) if c in _proxy_all_rets.columns]
_credit_names    = [c for c in CDX if c in _proxy_all_rets.columns]
_rc_names        = _rate_names + _credit_names
_risk_names      = [c for c in RISK_PROXY if c in _proxy_risk_rets.columns]

_avail_hedge = [i for i in ALL_PROXY if i in EOD_PRICES.columns]
_ph          = _prices_filled[_avail_hedge]
_p0h         = _ph.iloc[0]
_diffs_h     = _ph.diff().iloc[1:]

# Pre-scale unit PnL columns for hedge (loaded once, reused every test case)
_R_cols = []
_R_data = []
for _inst in _avail_hedge:
    if _inst in PAR_QUOTED:
        _col = (_diffs_h[_inst] / 100.0).values
    else:
        _d = float(_p0h[_inst])
        _col = (_diffs_h[_inst] / _d if _d != 0 else _diffs_h[_inst] * 0.0).values
    _R_cols.append(_inst)
    _R_data.append(_col)

_R_full   = np.array(_R_data, dtype=float).T            # shape (T_hedge, M)
_R_idx    = _diffs_h.index

_cost_full = np.array([
    COST_BPS[i] / 1e4 if i not in PAR_QUOTED
    else (float(_p0h[i]) / 100.0) * COST_BPS[i] / 1e4
    for i in _R_cols
], dtype=float)

# ------------------------------------------------------------------
# Cache path
# ------------------------------------------------------------------

_CACHE = "./model_cache.json"

# ------------------------------------------------------------------
# Fitting helpers
# ------------------------------------------------------------------

def _ewma_w(T, hl):
    lam = np.exp(-np.log(2.0) / hl)
    w   = lam ** np.arange(T - 1, -1, -1)
    return w / w.sum()

def _fit(X, y, hl):
    if hl is not None:
        sw = np.sqrt(_ewma_w(len(y), hl))
        return np.linalg.lstsq(X * sw[:, None], y * sw, rcond=None)[0]
    return np.linalg.lstsq(X, y, rcond=None)[0]

def _oos_mape(y, prices, X, cols, hl, frac=0.60):
    n  = len(y)
    t0 = int(n * frac)
    Xk = X[:, cols]
    st = max(5, (n - t0) // 5)
    rs, ns = [], []
    for sp in range(t0, n - 1, st):
        w   = _fit(Xk[:sp], y[:sp], hl)
        end = min(sp + st, n)
        rs.append(Xk[sp:end] @ w)
        ns.append(prices[sp + 1: end + 1])
    if not rs:
        return 999.0
    nav_p = prices[t0] * np.cumprod(1 + np.concatenate(rs))
    nav_a = np.concatenate(ns)[:len(nav_p)]
    return float(np.mean(np.abs(nav_p - nav_a) / nav_a) * 100)

def _oos_r2(y, X, cols, hl, frac=0.60):
    n  = len(y)
    t0 = int(n * frac)
    Xk = X[:, cols]
    st = max(5, (n - t0) // 5)
    ps, as_ = [], []
    for sp in range(t0, n - 1, st):
        w   = _fit(Xk[:sp], y[:sp], hl)
        end = min(sp + st, n)
        ps.append(Xk[sp:end] @ w)
        as_.append(y[sp:end])
    if not ps:
        return -999.0
    p = np.concatenate(ps); a = np.concatenate(as_)
    return float(1.0 - np.sum((a - p) ** 2) / (np.sum((a - a.mean()) ** 2) + 1e-12))

def _best_mape(y, prices, X, frac=0.60):
    M      = X.shape[1]
    corrs  = np.array([abs(np.corrcoef(X[:, j], y)[0, 1]) for j in range(M)])
    order  = np.argsort(corrs)[::-1]
    hls    = [None, 120]
    best   = (999.0, [order[0]], None)
    for hl in hls:
        for k in range(1, M + 1):
            cols = list(order[:k])
            m    = _oos_mape(y, prices, X, cols, hl, frac)
            if m < best[0]:
                best = (m, cols, hl)
    return best[1], best[2]

def _best_r2(y, X, frac=0.60):
    M      = X.shape[1]
    corrs  = np.array([abs(np.corrcoef(X[:, j], y)[0, 1]) for j in range(M)])
    order  = np.argsort(corrs)[::-1]
    hls    = [None, 120]
    best   = (-999.0, list(range(M)), None)
    for hl in hls:
        for k in range(1, M + 1):
            cols = list(order[:k])
            r    = _oos_r2(y, X, cols, hl, frac)
            if r > best[0]:
                best = (r, cols, hl)
    return best[1], best[2]

# ------------------------------------------------------------------
# NAV + Risk computation (expensive, cached to disk after first run)
# ------------------------------------------------------------------

def _compute_models():
    X_pe   = _proxy_all_rets[_proxy_etf_names].values.astype(float)
    X_rc   = _proxy_all_rets[_rc_names].values.astype(float)
    X_risk = _proxy_risk_rets[_risk_names].values.astype(float)

    nav_out  = {}
    risk_out = {}

    for etf in TARGET_ETFS:
        y      = _target_rets[etf].values.astype(float)
        prices = _prices_filled[etf].values

        # --- NAV: proxy ETF subset ---
        cols1, hl1 = _best_mape(y, prices, X_pe)
        Xk1   = X_pe[:, cols1]
        w1    = _fit(Xk1, y, hl1)
        weights = {_proxy_etf_names[c]: float(w) for c, w in zip(cols1, w1)}

        # --- NAV: rate/credit residual (only if OOS R2 > 3%) ---
        resid = y - Xk1 @ w1
        r2_rc = _oos_r2(resid, X_rc, list(range(X_rc.shape[1])), hl1)
        if r2_rc > 0.03:
            w_rc      = _fit(X_rc, resid, hl1)
            col_stds  = X_rc.std(axis=0)
            scaled    = np.abs(w_rc) * col_stds
            threshold = scaled.max() * 0.10
            for i, name in enumerate(_rc_names):
                if scaled[i] >= threshold:
                    weights[name] = weights.get(name, 0.0) + float(w_rc[i])

        nav_out[etf] = weights

        # --- Risk ---
        cols2, hl2 = _best_r2(y, X_risk)
        Xk2   = X_risk[:, cols2]
        w2    = _fit(Xk2, y, hl2)
        risk_out[etf] = {_risk_names[c]: float(w) for c, w in zip(cols2, w2)}

    return nav_out, risk_out

def _load_or_build():
    # Check if cached weights exist and match the current data shape
    data_sig = f"{EOD_PRICES.shape}_{float(EOD_PRICES.iloc[-1].sum()):.4f}"
    if os.path.exists(_CACHE):
        try:
            with open(_CACHE, "r") as f:
                stored = json.load(f)
            if stored.get("sig") == data_sig:
                return stored["nav"], stored["risk"]
        except Exception:
            pass

    nav_w, risk_w = _compute_models()

    try:
        with open(_CACHE, "w") as f:
            json.dump({"sig": data_sig, "nav": nav_w, "risk": risk_w}, f,
                      separators=(",", ":"))
    except Exception:
        pass

    return nav_w, risk_w

# ------------------------------------------------------------------
# Build nav_model dataframe from cached weights
# ------------------------------------------------------------------

def build_nav_model(nav_w):
    out = pd.DataFrame(0.0, index=TARGET_ETFS,
                       columns=[c for c in ALL_PROXY if c in _proxy_all_rets.columns])
    out.index.name = "ETF"
    for etf, weights in nav_w.items():
        for col, wt in weights.items():
            if col in out.columns:
                out.loc[etf, col] = wt
    out[out.abs() < 1e-10] = 0.0
    return out

# ------------------------------------------------------------------
# Build risk_model dataframe from cached weights
# ------------------------------------------------------------------

def build_risk_model(risk_w):
    out = pd.DataFrame(0.0, index=TARGET_ETFS, columns=RISK_PROXY)
    out.index.name = "ETF"
    for etf, weights in risk_w.items():
        for col, wt in weights.items():
            if col in out.columns:
                out.loc[etf, col] = wt
    out[out.abs() < 1e-10] = 0.0
    return out

# ------------------------------------------------------------------
# Hedge basket (always computed per test case, uses pre-built R matrix)
# ------------------------------------------------------------------

_SMOOTH = 1e-6

def _sabs(w):  return np.sqrt(w * w + _SMOOTH)
def _sgrad(w): return w / np.sqrt(w * w + _SMOOTH)

def _hedge_solve(p, R, cvec, maxn):
    from scipy.optimize import minimize
    T, M  = R.shape
    sig   = float(np.std(p))
    lam   = sig * 0.05
    RtR   = R.T @ R / (T - 1)
    Rtp   = R.T @ p / (T - 1)

    def fg(w):
        net   = p + R @ w
        res   = net - net.mean()
        var   = float(res @ res) / (T - 1)
        cost  = float(cvec @ _sabs(w))
        dv    = 2.0 * (RtR @ w + Rtp)
        dc    = lam * cvec * _sgrad(w)
        return var + lam * cost, dv + dc

    bounds = [(-maxn, maxn)] * M
    opts   = {"maxiter": 5000, "ftol": 1e-15, "gtol": 1e-10}
    starts = [np.zeros(M)]
    try:
        starts.append(np.clip(np.linalg.lstsq(R, -p, rcond=None)[0], -maxn, maxn))
    except Exception:
        pass
    rng = np.random.default_rng(99)
    for _ in range(5):
        starts.append(rng.standard_normal(M) * sig * 8.0)

    best_w, best_v = None, np.inf
    for w0 in starts:
        try:
            res = minimize(fg, np.clip(w0, -maxn, maxn),
                           method="L-BFGS-B", jac=True, bounds=bounds, options=opts)
            if res.fun < best_v:
                best_v = res.fun
                best_w = res.x.copy()
        except Exception:
            continue
    return best_w

def build_hedging_basket(portfolio):
    mock = pd.DataFrame({"Notional": [-200.0, 150.0, 800.0, 500.0, 1000.0]},
                        index=["TU", "FV", "TY", "US", "CDX_IG_5Y"])
    mock.index.name = "Instrument"

    if not portfolio:
        return mock

    port = {k: float(v) for k, v in portfolio.items()
            if k in TARGET_ETFS and k in EOD_PRICES.columns}
    if not port:
        return mock

    port_pnl = compute_pnl_series(port, _prices_filled[list(port.keys())])
    if port_pnl.empty or port_pnl.std() < 1e-10:
        return mock

    common = _R_idx.intersection(port_pnl.index)
    if len(common) < 20:
        return mock

    r_mask = _R_idx.isin(common)
    p_vec  = port_pnl.loc[common].values.astype(float)
    R_sub  = _R_full[r_mask]

    ok = np.isfinite(p_vec) & np.isfinite(R_sub).all(axis=1)
    p_vec, R_sub = p_vec[ok], R_sub[ok]
    if len(p_vec) < 10:
        return mock

    # Pre-select top-20 instruments by |corr(col, portfolio_pnl)|
    corrs   = np.array([abs(np.corrcoef(R_sub[:, j], p_vec)[0, 1])
                        for j in range(R_sub.shape[1])])
    top     = np.argsort(corrs)[::-1][:20]
    R_top   = R_sub[:, top]
    c_top   = _cost_full[top]
    i_top   = [_R_cols[j] for j in top]

    maxn = max(max(abs(v) for v in port.values()) * 10.0, 500.0)
    w    = _hedge_solve(p_vec, R_top, c_top, maxn)

    if w is None:
        try:
            w = np.linalg.lstsq(R_top, -p_vec, rcond=None)[0]
        except Exception:
            w = np.zeros(len(i_top))

    dust   = max(1e-3, max(abs(v) for v in port.values()) * 0.01)
    result = {i_top[j]: float(w[j]) for j in range(len(i_top)) if abs(w[j]) > dust}
    if not result:
        bj     = int(np.argmax(np.abs(w)))
        result = {i_top[bj]: float(w[bj])}

    out = pd.DataFrame.from_dict(result, orient="index", columns=["Notional"])
    out.index.name = "Instrument"
    return out

#######################################################################################################################
# YOUR CODE ENDS HERE
#######################################################################################################################

# Read test input
try:
    raw        = sys.stdin.read().strip()
    test_input = json.loads(raw) if raw else {}
except Exception:
    test_input = {}

portfolio = test_input.get("portfolio", {})

# Load (or build and cache) static models
_nav_w, _risk_w = _load_or_build()

nav_model      = build_nav_model(_nav_w)
risk_model     = build_risk_model(_risk_w)
hedging_basket = build_hedging_basket(portfolio)

#######################################################################################################################
# SUBMISSION FORMAT - DO NOT MODIFY/DELETE THE BELOW CODE
#######################################################################################################################

submission = {
    "nav":   nav_model.to_csv(),
    "risk":  risk_model.to_csv(),
    "hedge": hedging_basket.to_csv()
}

final_output = json.dumps(submission)
print(final_output)
