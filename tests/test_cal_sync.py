"""Test: calibration values sync correctly between UI and simulated ESP32 broadcast.
Verifies that userSet flag is properly cleared after Set buttons are clicked,
allowing the next broadcast to confirm the saved value.
"""
import asyncio
import json
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from playwright.async_api import async_playwright

UI_URL = "http://localhost:8080"
NUM_CHANNELS = 4  # demo mode creates 4 channels
PASS = 0
FAIL = 0

def ok(msg):
    global PASS; PASS += 1
    print(f"  [PASS] {msg}")

def fail(msg):
    global FAIL; FAIL += 1
    print(f"  [FAIL] {msg}")

async def evaluate(page, code):
    return await page.evaluate(code)

async def setup_demo_mode(page):
    await page.goto(UI_URL, wait_until="domcontentloaded")
    await asyncio.sleep(0.3)
    await page.locator("#demoMode").check()
    await page.locator("#connectPanel .btn-primary").click()
    await page.wait_for_timeout(2000)
    assert await page.locator("#dashboard").is_visible(), "Dashboard should be visible"
    ok("Demo mode active")

async def inject_broadcast(page, overrides=None):
    data = {
        "v": 230.0,
        "voltageCalibration": 260.0,
        "currentCalibration": [100.0]*NUM_CHANNELS,
        "noiseFloor": [0.0]*NUM_CHANNELS,
        "rmsSamples": 1000,
        "ch": [
            {"n": f"Ch{i+1}", "a": 1.0+i*0.5, "w": 200+i*20, "va": 230+i*10,
             "pf": 0.95, "kwh": 10.0+i, "s": 0, "r": True, "cl": 16.0, "pl": 3500, "mkwh": 48.0}
            for i in range(NUM_CHANNELS)
        ],
        "events": []
    }
    if overrides:
        data.update(overrides)
    return await evaluate(page, f"""
        (() => {{
            updateDashboard({json.dumps(data)});
            return 'ok';
        }})()
    """)

async def expand_cal_channel(page, idx):
    """Ensure a channel's calibration section is expanded by evaluating JS."""
    await evaluate(page, f"""
        (() => {{
            const headers = document.querySelectorAll('.cal-collapse-header');
            if (headers[{idx}]) {{
                const body = headers[{idx}].nextElementSibling;
                if (body && !body.classList.contains('open')) {{
                    headers[{idx}].click();
                }}
            }}
            return 'ok';
        }})()
    """)
    await asyncio.sleep(0.1)

async def get_cal_input_val(page, el_id):
    exists = await evaluate(page, f"!!document.getElementById('{el_id}')")
    if not exists:
        return None
    return await evaluate(page, f"document.getElementById('{el_id}').value")

async def test_send_and_sync(page):
    print("\n--- Test 1: Current Calibration Set + Sync ---")
    await inject_broadcast(page, {"currentCalibration": [100.0]*NUM_CHANNELS})
    await asyncio.sleep(0.3)

    for i in range(NUM_CHANNELS):
        val = await get_cal_input_val(page, f"currCal_{i}")
        if val is None:
            fail(f"currCal_{i} element not found")
        elif val == "100.0":
            ok(f"currCal_{i} initial = {val}")
        else:
            fail(f"currCal_{i} expected 100.0, got {val}")

    await expand_cal_channel(page, 0)

    inp = page.locator("#currCal_0")
    await inp.fill("250.0")
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('currCal_0').dataset.userSet === 'true'")
    ok(f"currCal_0 userSet {'set' if user_set else 'NOT set'} after typing")

    set_btn = page.locator("#currCal_0 + button")
    await set_btn.click()
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('currCal_0').dataset.userSet === 'true'")
    if not user_set:
        ok("currCal_0 userSet cleared after Set click")
    else:
        fail("currCal_0 userSet still set after Set click")

    await inject_broadcast(page, {"currentCalibration": [250.0] + [100.0]*(NUM_CHANNELS-1)})
    await asyncio.sleep(0.3)

    val = await get_cal_input_val(page, "currCal_0")
    if val == "250.0":
        ok(f"currCal_0 synced to {val} after broadcast")
    else:
        fail(f"currCal_0 expected 250.0, got {val}")

async def test_rms_samples_sync(page):
    print("\n--- Test 2: RMS Samples Set + Sync ---")
    await inject_broadcast(page, {"rmsSamples": 1000})
    await asyncio.sleep(0.3)

    val = await evaluate(page, "document.getElementById('rmsSamples').value")
    if val == "1000":
        ok(f"rmsSamples initial = {val}")
    else:
        fail(f"rmsSamples expected 1000, got {val}")

    rms_inp = page.locator("#rmsSamples")
    await rms_inp.fill("500")
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('rmsSamples').dataset.userSet === 'true'")
    ok(f"rmsSamples userSet {'set' if user_set else 'NOT set'} after typing")

    await page.locator("#rmsSamples + button").click()
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('rmsSamples').dataset.userSet === 'true'")
    if not user_set:
        ok("rmsSamples userSet cleared after Set click")
    else:
        fail("rmsSamples userSet still set after Set click")

    await inject_broadcast(page, {"rmsSamples": 500})
    await asyncio.sleep(0.3)

    val = await evaluate(page, "document.getElementById('rmsSamples').value")
    if val == "500":
        ok(f"rmsSamples synced to {val} after broadcast")
    else:
        fail(f"rmsSamples expected 500, got {val}")

async def test_voltage_cal_sync(page):
    print("\n--- Test 3: Voltage Calibration Set + Sync ---")
    await inject_broadcast(page, {"voltageCalibration": 260.0})
    await asyncio.sleep(0.3)

    val = await evaluate(page, "document.getElementById('voltCal').value")
    if val == "260.0":
        ok(f"voltCal initial = {val}")
    else:
        fail(f"voltCal expected 260.0, got {val}")

    vc_inp = page.locator("#voltCal")
    await vc_inp.fill("240.0")
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('voltCal').dataset.userSet === 'true'")
    ok(f"voltCal userSet {'set' if user_set else 'NOT set'} after typing")

    await page.locator("#voltCal + button").click()
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('voltCal').dataset.userSet === 'true'")
    if not user_set:
        ok("voltCal userSet cleared after Set click")
    else:
        fail("voltCal userSet still set after Set click")

    await inject_broadcast(page, {"voltageCalibration": 240.0})
    await asyncio.sleep(0.3)

    val = await evaluate(page, "document.getElementById('voltCal').value")
    if val == "240.0":
        ok(f"voltCal synced to {val} after broadcast")
    else:
        fail(f"voltCal expected 240.0, got {val}")

async def test_noise_floor_auto_zero(page):
    print("\n--- Test 4: Noise Floor Auto-Zero Sync ---")
    await inject_broadcast(page, {"noiseFloor": [0.0]*NUM_CHANNELS})
    await asyncio.sleep(0.3)

    val = await get_cal_input_val(page, "nf_0")
    if val is None:
        fail("nf_0 element not found")
        return

    if val == "0.000":
        ok(f"nf_0 initial = {val}")
    else:
        fail(f"nf_0 expected 0.000, got {val}")

    await expand_cal_channel(page, 0)

    nf_inp = page.locator("#nf_0")
    await nf_inp.fill("0.500")
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('nf_0').dataset.userSet === 'true'")
    ok(f"nf_0 userSet {'set' if user_set else 'NOT set'} after typing")

    await page.locator("#nf_0 + button").click()
    await asyncio.sleep(0.1)

    user_set = await evaluate(page, "document.getElementById('nf_0').dataset.userSet === 'true'")
    if not user_set:
        ok("nf_0 userSet cleared after Auto-Zero click")
    else:
        fail("nf_0 userSet still set after Auto-Zero click")

    await inject_broadcast(page, {"noiseFloor": [0.482] + [0.0]*(NUM_CHANNELS-1)})
    await asyncio.sleep(0.3)

    val = await get_cal_input_val(page, "nf_0")
    if val == "0.482":
        ok(f"nf_0 synced to {val} after auto-zero broadcast")
    else:
        fail(f"nf_0 expected 0.482, got {val}")

async def test_nvs_reset_clears_user_set(page):
    print("\n--- Test 5: NVS Reset Clears userSet ---")
    await inject_broadcast(page, {
        "currentCalibration": [100.0]*NUM_CHANNELS,
        "noiseFloor": [0.0]*NUM_CHANNELS,
        "rmsSamples": 1000,
        "voltageCalibration": 260.0
    })
    await asyncio.sleep(0.3)

    await evaluate(page, """
        (() => {
            document.getElementById('rmsSamples').dataset.userSet = 'true';
            document.getElementById('voltCal').dataset.userSet = 'true';
            for (let i = 0; i < 4; i++) {
                const nf = document.getElementById('nf_' + i);
                if (nf) nf.dataset.userSet = 'true';
                const cc = document.getElementById('currCal_' + i);
                if (cc) cc.dataset.userSet = 'true';
            }
            return 'ok';
        })()
    """)
    await asyncio.sleep(0.1)

    nvs_btn = page.locator("#resetNvsBtn")
    await nvs_btn.click()
    await asyncio.sleep(0.2)
    await nvs_btn.click()
    await asyncio.sleep(0.2)

    all_cleared = True
    for check_id in ["rmsSamples", "voltCal"]:
        s = await evaluate(page, f"document.getElementById('{check_id}').dataset.userSet === 'true'")
        if s:
            fail(f"{check_id} userSet not cleared")
            all_cleared = False

    for i in range(NUM_CHANNELS):
        for prefix in ["nf", "currCal"]:
            el_id = f"{prefix}_{i}"
            exists = await evaluate(page, f"!!document.getElementById('{el_id}')")
            if not exists: continue
            s = await evaluate(page, f"document.getElementById('{el_id}').dataset.userSet === 'true'")
            if s:
                fail(f"{el_id} userSet not cleared")
                all_cleared = False

    if all_cleared:
        ok("All calibration inputs userSet cleared after NVS reset")

async def test_broadcast_preserves_typing(page):
    print("\n--- Test 6: Broadcast Preserves Active Typing ---")
    await inject_broadcast(page, {"rmsSamples": 1000})
    await asyncio.sleep(0.3)

    rms_inp = page.locator("#rmsSamples")
    await rms_inp.focus()
    await rms_inp.fill("750")
    await asyncio.sleep(0.1)

    await inject_broadcast(page, {"rmsSamples": 999})
    await asyncio.sleep(0.3)

    val = await evaluate(page, "document.getElementById('rmsSamples').value")
    if val == "750":
        ok("Broadcast did not overwrite active user typing (userSet + activeElement)")
    else:
        fail(f"Expected 750 (user typed), got {val}")

async def main():
    global PASS, FAIL
    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True, args=["--no-sandbox"])
        ctx = await browser.new_context(viewport={"width": 1280, "height": 900})
        page = await ctx.new_page()
        page.on("pageerror", lambda err: print(f"  [PAGE_ERROR] {err}"))

        print("=" * 55)
        print("  Calibration Sync Loop Test")
        print("=" * 55)

        await setup_demo_mode(page)
        await test_send_and_sync(page)
        await test_rms_samples_sync(page)
        await test_voltage_cal_sync(page)
        await test_noise_floor_auto_zero(page)
        await test_nvs_reset_clears_user_set(page)
        await test_broadcast_preserves_typing(page)

        print("\n" + "=" * 55)
        print(f"  RESULTS: {PASS} passed, {FAIL} failed")
        print("=" * 55)

        await page.screenshot(path="test_cal_sync.png", full_page=True)
        await browser.close()
        if FAIL > 0:
            sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())
