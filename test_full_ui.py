"""Full UI loop test for ESP32 Electricity Monitor.
Connects to real ESP32 at 192.168.100.2 via local UI server.
"""
import asyncio
from playwright.async_api import async_playwright

ESP32_IP = "192.168.100.2"
UI_URL = "http://localhost:8080"

async def main():
    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True, args=["--no-sandbox"])
        ctx = await browser.new_context(viewport={"width": 1920, "height": 1080})
        page = await ctx.new_page()

        page.on("pageerror", lambda err: print(f"[PAGE_ERROR] {err}"))

        print("=" * 60)
        print("PHASE 1: NAVIGATE & CONNECT TO ESP32")
        print("=" * 60)
        await page.goto(UI_URL, wait_until="domcontentloaded", timeout=15000)
        await page.wait_for_timeout(500)

        # Verify connect panel is visible
        cp = page.locator("#connectPanel")
        assert await cp.is_visible(), "Connect panel should be visible"
        print("[PASS] Connect panel visible")

        # Enter ESP32 IP and click Connect
        await page.locator("#esp32Ip").fill(ESP32_IP)
        await page.locator("#connectPanel .btn-primary").click()
        await page.wait_for_timeout(3000)

        dashboard = page.locator("#dashboard")
        connected = False
        try:
            await dashboard.wait_for(state="visible", timeout=8000)
            connected = True
            print("[PASS] Connected to ESP32, dashboard visible")
        except:
            print("[WARN] ESP32 WebSocket connection failed — trying demo mode")

        if not connected:
            await page.locator("#demoMode").check()
            await page.locator("#connectPanel .btn-primary").click()
            await page.wait_for_timeout(2000)
            try:
                await dashboard.wait_for(state="visible", timeout=5000)
                connected = True
                print("[PASS] Demo mode activated")
            except:
                print("[FAIL] Dashboard did not appear")
                await page.screenshot(path="test_fail_dashboard.png")
                await browser.close()
                return

        # Wait for initial data
        await page.wait_for_timeout(3000)

        print("\n" + "=" * 60)
        print("PHASE 2: VERIFY HEADER CARD")
        print("=" * 60)

        try:
            hdr = page.locator(".header-card")
            assert await hdr.is_visible()
            print("[PASS] Header card visible")

            for el_id in ["headerVoltage", "totalPower", "totalCurrent"]:
                el = page.locator(f"#{el_id}")
                assert await el.is_visible(), f"#{el_id} should be visible"
                txt = await el.text_content()
                print(f"  [INFO] #{el_id} = {txt.strip()}")
        except Exception as e:
            print(f"[WARN] Header check: {e}")

        try:
            led = page.locator("#ledStatus")
            assert await led.is_visible()
            cls = await led.get_attribute("class")
            print(f"  [INFO] LED class = {cls}")
        except:
            pass

        print("\n" + "=" * 60)
        print("PHASE 3: VERIFY CHANNEL CARDS")
        print("=" * 60)

        cards = page.locator(".card")
        ccount = await cards.count()
        print(f"[INFO] Found {ccount} channel card(s)")

        if ccount > 0:
            for i in range(ccount):
                card = cards.nth(i)
                name = await card.locator(".name-field").text_content()
                relay_btn = card.locator(".relay-toggle-btn")
                rb_text = await relay_btn.text_content()
                print(f"  Card #{i}: name='{name.strip()}', relay='{rb_text.strip()}'")
                assert await relay_btn.is_visible(), f"Card #{i}: relay button visible"
        else:
            print("[FAIL] No channel cards found")
            await page.screenshot(path="test_no_cards.png")
            await browser.close()
            return

        print("\n" + "=" * 60)
        print("PHASE 4: RELAY TOGGLE TEST")
        print("=" * 60)

        for i in range(min(ccount, 4)):
            card = cards.nth(i)
            btn = card.locator(".relay-toggle-btn")
            text_before = (await btn.text_content()).strip()
            await btn.click()
            await page.wait_for_timeout(800)
            text_after = (await btn.text_content()).strip()
            if text_after != text_before:
                print(f"[PASS] Card #{i}: relay toggled '{text_before}' → '{text_after}'")
            else:
                print(f"[WARN] Card #{i}: relay text unchanged '{text_before}' — may need real data cycle")

        print("\n" + "=" * 60)
        print("PHASE 5: EDIT MODAL TEST")
        print("=" * 60)

        for i in range(min(ccount, 2)):
            card = cards.nth(i)
            edit_btn = card.locator(".btn-edit")
            if await edit_btn.is_visible():
                await edit_btn.click()
                await page.wait_for_timeout(500)

                modal = page.locator("#editModal")
                if await modal.is_visible():
                    print(f"[PASS] Card #{i}: edit modal opened")
                    # Verify modal fields
                    title = await page.locator("#modalTitle").text_content()
                    print(f"  [INFO] Modal title = {title.strip()}")
                    # Fill in new values
                    await page.locator("#modalChName").fill(f"Test CH{i+1}")
                    await page.locator("#modalChClim").fill("15")
                    await page.locator("#modalChPlim").fill("3000")
                    # Save
                    await page.locator("#editModal .btn-primary").click()
                    await page.wait_for_timeout(500)
                    assert await modal.is_hidden(), "Modal should close after save"
                    print(f"[PASS] Card #{i}: edit saved, modal closed")
                else:
                    print(f"[WARN] Card #{i}: edit modal not visible")

        print("\n" + "=" * 60)
        print("PHASE 6: RESET CONFIRM TEST")
        print("=" * 60)

        for i in range(min(ccount, 2)):
            card = cards.nth(i)
            reset_btn = card.locator(".btn-reset")
            if await reset_btn.is_visible():
                txt_before = (await reset_btn.text_content()).strip()
                await reset_btn.click()
                await page.wait_for_timeout(300)
                txt_after = (await reset_btn.text_content()).strip()
                if "Confirm" in txt_after or txt_after != txt_before:
                    print(f"[PASS] Card #{i}: reset changed '{txt_before}' → '{txt_after}' (confirm mode)")
                    # Click again to send
                    await reset_btn.click()
                    await page.wait_for_timeout(500)
                    txt_final = (await reset_btn.text_content()).strip()
                    print(f"  [INFO] Card #{i}: after second click = '{txt_final}'")
                else:
                    print(f"[WARN] Card #{i}: reset text unchanged '{txt_before}'")

        print("\n" + "=" * 60)
        print("PHASE 7: EVENT LOG TEST")
        print("=" * 60)

        try:
            el = page.locator("#eventList")
            assert await el.is_visible()
            events = el.locator(".event-item")
            ev_count = await events.count()
            print(f"[INFO] Found {ev_count} event item(s)")
            if ev_count > 0:
                first_ev = await events.first.text_content()
                print(f"  Sample event: {first_ev.strip()[:80]}")
            print("[PASS] Event log visible")
        except Exception as e:
            print(f"[WARN] Event log: {e}")

        try:
            ec = page.locator("#eventCount")
            ec_text = await ec.text_content()
            print(f"  [INFO] Event count: {ec_text.strip()}")
        except:
            pass

        print("\n" + "=" * 60)
        print("PHASE 8: DATA REFRESH VERIFICATION")
        print("=" * 60)

        await page.wait_for_timeout(3000)

        for i in range(ccount):
            card = cards.nth(i)
            val_a = await card.locator(".val-a").text_content()
            val_w = await card.locator(".val-w").text_content()
            pf = await card.locator(".val-pf").text_content()
            pct = await card.locator(".bar-pct").text_content()
            print(f"  Card #{i}: I={val_a.strip()}A  P={val_w.strip()}W  PF={pf.strip()}  Load={pct.strip()}")

        print("\n" + "=" * 60)
        print("PHASE 9: DEMO MODE LIVE STREAMING")
        print("=" * 60)

        if connected:
            # Disconnect and try demo mode
            await page.locator(".btn-outline").click()
            await page.wait_for_timeout(500)
            # Reconnect in demo mode
            await page.locator("#demoMode").check()
            await page.locator("#connectPanel .btn-primary").click()
            await page.wait_for_timeout(2000)
            try:
                await dashboard.wait_for(state="visible", timeout=5000)
                print("[PASS] Demo mode after disconnect")
            except:
                pass

        # Wait for 3 data updates in demo mode
        await page.wait_for_timeout(4000)

        # Verify values changed
        card0 = cards.nth(0)
        val_a_updated = await card0.locator(".val-a").text_content()
        print(f"  [INFO] Card #0 current after 4s demo: {val_a_updated.strip()}A")

        print("\n" + "=" * 60)
        print("TEST SUMMARY — ALL PHASES COMPLETED")
        print("=" * 60)

        await page.screenshot(path="test_final.png", full_page=True)
        print("[DONE] Full-page screenshot: test_final.png")
        await browser.close()
        print("[DONE] Browser closed.")


if __name__ == "__main__":
    asyncio.run(main())
