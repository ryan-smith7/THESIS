import numpy as np

# ── Raw calibration data ─────────────────────────────────────────────────────
# ADC mean counts (12-bit, ESP32 ADC1 ch7, GAIN_1_4, Zephyr 4.2)
# Potting mix, water-added method, m_dry = 105g baseline
# Sample 1 (ADC 2371, 10.48%) REMOVED — replaced with dry anchor at 0%
# Sample 10 (ADC 1035, 126.67%) REMOVED — supersaturated, outside sensor range

adc = np.array([
    2371.0,   # DRY ANCHOR — dry air ADC reading = 0% moisture
    2326.0,   # sample 2  — m_wet=134g
    2204.0,   # sample 3  — m_wet=144g
    2132.6,   # sample 4  — m_wet=157g
    1938.4,   # sample 5  — m_wet=167g
    1833.4,   # sample 6  — m_wet=182g
    1345.4,   # sample 7  — m_wet=192g
    1152.2,   # sample 8  — m_wet=200g
    1049.4,   # sample 9  — m_wet=212g
])

# Gravimetric moisture: theta_g = (m_wet - m_dry) / m_dry * 100
# Dry anchor set to 0.0 manually
m_wet = np.array([0, 134, 144, 157, 167, 182, 192, 200, 212], dtype=float)
m_dry = 105.0

theta_g = np.array([0.00,  # dry anchor — hardcoded
    *((m_wet[1:] - m_dry) / m_dry * 100.0)
])

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
labels = ["DRY ANCHOR", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9"]
print(f"\n{'Sample':>12}  {'ADC':>7}  {'Measured':>10}  {'Predicted':>10}  {'Residual':>9}")
for label, v, m, p in zip(labels, adc, theta_g, predicted):
    print(f"  {label:>10}  {v:>7.1f}  {m:>10.2f}  {p:>10.2f}  {m-p:>+9.2f}")

# ── Sanity checks at key ADC values ─────────────────────────────────────────
print("\nSanity checks:")
for v_test, desc in [(2371, "dry anchor"), (2200, "slightly dry"),
                     (1938, "mid range"), (1049, "near saturated")]:
    p = np.polyval(coeffs, v_test)
    print(f"  ADC={v_test:>4} ({desc:>15}): {p:.2f}%")

# ── Firmware defines ─────────────────────────────────────────────────────────
print(f"""
Firmware defines:
#define CONFIG_MOISTURE_DRY_ADC       2371       /* clamp floor — above = 0% */
#define CONFIG_MOISTURE_COEFF_A3  {a3:.10f}f
#define CONFIG_MOISTURE_COEFF_A2  {a2:.8f}f
#define CONFIG_MOISTURE_COEFF_A1  {a1:.8f}f
#define CONFIG_MOISTURE_COEFF_A0  {a0:.6f}f
""")

# ── Curve points for plotting ────────────────────────────────────────────────
adc_curve   = np.linspace(950, 2450, 200)
theta_curve = np.clip(np.polyval(coeffs, adc_curve), 0, 140)