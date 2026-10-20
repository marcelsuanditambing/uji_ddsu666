"""Perhitungan energi dan penalti kVArh (TDB-APFC-01 Subbab 7.5).

Penghitung energi dari ESP32 bersifat kumulatif. Untuk satu periode, energi = jumlah
selisih positif antar-sampel berurutan. Selisih negatif (misalnya setelah perintah
energy_reset) diabaikan sehingga reset di tengah periode tidak merusak hasil.
"""

LIMIT_RATIO = 0.62   # batas kVArh/kWh PLN (setara cos phi 0,85)

FIELDS = ("kwh", "kvarh_ind", "kvarh_cap", "kvarh_load")


def integrate(rows):
    """rows: iterable of dict/sqlite3.Row dengan kolom FIELDS, urut waktu."""
    total = {k: 0.0 for k in FIELDS}
    prev = None
    n = 0
    for r in rows:
        n += 1
        if prev is not None:
            for k in FIELDS:
                d = (r[k] or 0.0) - (prev[k] or 0.0)
                if d > 0:
                    total[k] += d
        prev = r
    total["samples"] = n
    return total


def penalty(d_kvarh, d_kwh, tariff, scale):
    """Kembalikan (kVArh lebih [skala], penalti Rp)."""
    over = max(0.0, d_kvarh - LIMIT_RATIO * d_kwh)
    return over * scale, over * scale * tariff


def summarize(rows, tariff, scale, t_from=None, t_to=None):
    e = integrate(rows)
    kwh, ind, cap, load = e["kwh"], e["kvarh_ind"], e["kvarh_cap"], e["kvarh_load"]
    over_with, rp_with = penalty(ind, kwh, tariff, scale)
    over_without, rp_without = penalty(load, kwh, tariff, scale)
    hours = (t_to - t_from) / 3600.0 if (t_from and t_to and t_to > t_from) else 0.0
    return {
        "samples": e["samples"],
        "hours": hours,
        "scale": scale,
        "tariff": tariff,
        "limit_ratio": LIMIT_RATIO,
        "kwh": kwh * scale,
        "kvarh_ind": ind * scale,
        "kvarh_cap": cap * scale,
        "kvarh_load": load * scale,
        "ratio_with": (ind / kwh) if kwh > 1e-9 else None,
        "ratio_without": (load / kwh) if kwh > 1e-9 else None,
        "over_with": over_with,
        "over_without": over_without,
        "penalty_with": rp_with,
        "penalty_without": rp_without,
        "saving": rp_without - rp_with,
        "saving_per_hour": (rp_without - rp_with) / hours if hours > 0.01 else None,
    }
