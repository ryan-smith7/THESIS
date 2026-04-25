import numpy as np

# ── Raw calibration data ─────────────────────────────────────────────────────
# ADC mean counts (12-bit, ESP32 ADC1 ch7, GAIN_1_4, Zephyr 4.2)
# Potting mix, water-added method, m_dry = 105g baseline
adc = np.array([
    2371.0,   # sample 1  — 0g added,  m_wet=116g
    2326.0,   # sample 2  — 3g added,  m_wet=134g  (wait — see note)
    2204.0,   # sample 3  — 6g added,  m_wet=144g
    2132.6,   # sample 4  — 11g added, m_wet=157g
    1938.4,   # sample 5  — 16g added, m_wet=167g
    1833.4,   # sample 6  — 22g added, m_wet=182g
    1345.4,   # sample 7  — 29g added, m_wet=192g
    1152.2,   # sample 8  — 38g added, m_wet=200g
    1049.4,   # sample 9  — 49g added, m_wet=212g
    1035.2,   # sample 10 — 63g added, m_wet=238g
])

# Gravimetric moisture: theta_g = (m_wet - m_dry) / m_dry * 100
m_wet = np.array([116, 134, 144, 157, 167, 182, 192, 200, 212, 238], dtype=float)
m_dry = 105.0

theta_g = (m_wet - m_dry) / m_dry * 100.0

print("theta_g (%):", np.round(theta_g, 2))

# ── 3rd-order polynomial fit (OLS via QR decomposition) ─────────────────────
# np.polyfit uses numpy's lstsq which calls LAPACK dgelsd internally.
# Input is normalised to avoid Vandermonde matrix ill-conditioning.
# Returns coefficients [a3, a2, a1, a0] for: theta = a3*V^3 + a2*V^2 + a1*V + a0
coeffs = np.polyfit(adc, theta_g, deg=3)
a3, a2, a1, a0 = coeffs

print(f"\nCoefficients:")
print(f"  a3 = {a3:.10f}")
print(f"  a2 = {a2:.8f}")
print(f"  a1 = {a1:.8f}")
print(f"  a0 = {a0:.6f}")

# ── Goodness of fit ──────────────────────────────────────────────────────────
predicted = np.polyval(coeffs, adc)

ss_res = np.sum((theta_g - predicted) ** 2)
ss_tot = np.sum((theta_g - np.mean(theta_g)) ** 2)
r2     = 1 - ss_res / ss_tot
rmse   = np.sqrt(np.mean((theta_g - predicted) ** 2))

print(f"\nGoodness of fit:")
print(f"  R²   = {r2:.6f}")
print(f"  RMSE = {rmse:.4f} %")

# ── Per-sample residuals ─────────────────────────────────────────────────────
print(f"\n{'Sample':>6}  {'ADC':>7}  {'Measured':>10}  {'Predicted':>10}  {'Residual':>9}")
for i, (v, m, p) in enumerate(zip(adc, theta_g, predicted)):
    print(f"  {i+1:>4}  {v:>7.1f}  {m:>10.2f}  {p:>10.2f}  {m-p:>+9.2f}")

# ── Firmware defines ─────────────────────────────────────────────────────────
print(f"""
Firmware defines:
#define CONFIG_MOISTURE_COEFF_A3  {a3:.10f}f
#define CONFIG_MOISTURE_COEFF_A2  {a2:.8f}f
#define CONFIG_MOISTURE_COEFF_A1  {a1:.8f}f
#define CONFIG_MOISTURE_COEFF_A0  {a0:.6f}f
""")

# ── Curve points for plotting (optional) ─────────────────────────────────────
adc_curve   = np.linspace(950, 2450, 200)
theta_curve = np.clip(np.polyval(coeffs, adc_curve), 0, 140)